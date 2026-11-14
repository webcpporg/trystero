// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PLATFORM_SEALER_HPP
#define WEBCPP_TRYSTERO_PLATFORM_SEALER_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/post.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/platform/entropy.hpp>
#include <webcpp/trystero/protocol/signal.hpp>

#include <webcpp/asio/any_io_executor.hpp>  // lint-world: posts handlers only

#ifdef WEBCPP_TARGET_EMSCRIPTEN
#include <webcpp/asio/bind_allocator.hpp>       // lint-world: binds an allocator only
#include <webcpp/asio/browser/async_await.hpp>  // lint-world: awaits the page's promises
#include <webcpp/asio/io_context.hpp>           // lint-world: names its executor only

#include <emscripten/em_asm.h>
#include <emscripten/val.h>
#include <boost/system/error_code.hpp>

#include <deque>
#else
#include <openssl/evp.h>
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace webcpp::trystero::platform {

namespace detail {

#ifdef WEBCPP_TARGET_EMSCRIPTEN

// The page's Web Crypto, called from C++. Each function is an EM_ASM block in an inline function,
// which Emscripten allows in several translation units, as webcpp::webrtc's and webcpp::beast's
// browser imports are. A Web Crypto call is made inside an async function, so that what it throws
// becomes the rejection of the promise it returns, and no JavaScript exception reaches a
// WebAssembly frame. The functions use Emval and HEAPU8, which webcpp::asio's browser runtime has
// Emscripten link, in every program on /webcpp/asio//asio.

// clang-format off
/** Copies the first `size` bytes of `bytes`, a `Uint8Array`, to `data`. */
// NOLINTNEXTLINE(misc-const-correctness): JavaScript writes to the pointed WebAssembly storage
inline void js_copy(emscripten::EM_VAL bytes, void* data, std::size_t size) {
    EM_ASM(({
    const bytes = Emval.toValue($0);
    const data = $1 >>> 0;
    const size = $2 >>> 0;

    HEAPU8.set(bytes.subarray(0, size), data);
    }), bytes, data, size);
}

/**
 * A promise of the AES-GCM key of the `size` raw bytes at `key`, from `crypto.subtle.importKey`,
 * rejected where there is no `crypto.subtle`.
 */
inline emscripten::EM_VAL js_import_key(const std::uint8_t* key, std::size_t size) {
    return reinterpret_cast<emscripten::EM_VAL>(EM_ASM_PTR(({
    const key = $0 >>> 0;
    const size = $1 >>> 0;

    const raw = HEAPU8.slice(key, key + size);
    return Emval.toHandle((async () => crypto.subtle.importKey('raw', raw, 'AES-GCM', false,
        ['encrypt', 'decrypt']))());
    }), key, size));
}

/**
 * A promise of `crypto.subtle.encrypt`, or `decrypt`, of the `size` bytes at `data` under `key`
 * and the IV at `iv`, rejected for what the call throws as for what it rejects.
 */
inline emscripten::EM_VAL js_crypt(bool decrypt, emscripten::EM_VAL key, const std::uint8_t* iv,
                                   std::size_t iv_size, const void* data, std::size_t size) {
    return reinterpret_cast<emscripten::EM_VAL>(EM_ASM_PTR(({
    const decrypt = $0;
    const key = Emval.toValue($1);
    const iv = HEAPU8.slice($2 >>> 0, ($2 >>> 0) + ($3 >>> 0));
    const data = HEAPU8.slice($4 >>> 0, ($4 >>> 0) + ($5 >>> 0));

    const algorithm = {name: 'AES-GCM', iv};
    return Emval.toHandle((async () => decrypt ? crypto.subtle.decrypt(algorithm, key, data)
        : crypto.subtle.encrypt(algorithm, key, data))());
    }), decrypt, key, iv, iv_size, data, size));
}
// clang-format on

/** Takes ownership of a handle an import returned. */
inline emscripten::val take(emscripten::EM_VAL handle) {
    return emscripten::val::take_ownership(handle);
}

/** A copy of an `ArrayBuffer`'s bytes, in `memory`. */
inline backend::bytes from_js(const emscripten::val& buffer, std::pmr::memory_resource* memory) {
    const emscripten::val view = emscripten::val::global("Uint8Array").new_(buffer);
    backend::bytes out(view["length"].as<std::size_t>(), memory);
    js_copy(view.as_handle(), out.data(), out.size());
    return out;
}

#else

/** Frees an OpenSSL cipher context. */
struct cipher_context_deleter {
    /** Frees `c`. */
    void operator()(EVP_CIPHER_CTX* c) const { EVP_CIPHER_CTX_free(c); }
};

/** An OpenSSL cipher context, freed with its owner. */
using cipher_context = std::unique_ptr<EVP_CIPHER_CTX, cipher_context_deleter>;

/** The size of AES-GCM's authentication tag, which WebCrypto appends to the ciphertext. */
inline constexpr std::size_t gcm_tag_size = 16;

/** AES-256-GCM with a 16-byte IV, the ciphertext followed by its 16-byte tag: what WebCrypto's
 * encrypt returns. */
inline std::optional<backend::bytes> aes_gcm(bool encrypt, const std::array<std::uint8_t, 32>& key,
                                             std::span<const std::uint8_t> iv,
                                             std::span<const std::uint8_t> input,
                                             std::pmr::memory_resource* memory) {
    if (!encrypt && input.size() < gcm_tag_size) {
        return std::nullopt;
    }
    const cipher_context context(EVP_CIPHER_CTX_new());
    if (!context) {
        return std::nullopt;
    }
    const auto body = encrypt ? input : input.first(input.size() - gcm_tag_size);
    backend::bytes out(body.size() + (encrypt ? gcm_tag_size : 0), memory);
    int length = 0;
    int final_length = 0;
    // OpenSSL's functions return 0 when they fail.
    bool ok = EVP_CipherInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr,
                                encrypt ? 1 : 0) != 0 &&
              EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN,
                                  static_cast<int>(iv.size()), nullptr) != 0 &&
              EVP_CipherInit_ex(context.get(), nullptr, nullptr, key.data(), iv.data(), -1) != 0 &&
              EVP_CipherUpdate(context.get(), out.data(), &length, body.data(),
                               static_cast<int>(body.size())) != 0;
    if (ok && !encrypt) {
        // OpenSSL takes the expected tag through a pointer to mutable bytes, and only reads it.
        auto* tag = const_cast<std::uint8_t*>(input.data() + body.size());
        ok = EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG,
                                 static_cast<int>(gcm_tag_size), tag) != 0;
    }
    ok = ok && EVP_CipherFinal_ex(context.get(), out.data() + length, &final_length) != 0;
    if (ok && encrypt) {
        ok = EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG,
                                 static_cast<int>(gcm_tag_size), out.data() + body.size()) != 0;
    }
    if (!ok) {
        return std::nullopt;
    }
    return out;
}

#endif

}  // namespace detail

#ifdef WEBCPP_TARGET_EMSCRIPTEN

/**
 * AES-GCM under one room key, in Trystero's sealed text form: on emscripten through the page's
 * `crypto.subtle`, under a key imported once, as Trystero's `encrypt` and `decrypt` do.
 *
 * Each Web Crypto promise is awaited with webcpp::asio's `browser::async_await`, which posts its
 * settlement to the executor, through the io_context's own executor when the executor is one, so
 * that the wait and the settlement's post are both in the room's resource. The text is copied into
 * JavaScript during the call; an operation that arrives while the key is still being imported waits
 * with a copy of its text, in the room's resource. Without `crypto.subtle`, outside a secure
 * context, every operation fails. A seal's IV comes from the platform's one source of randomness,
 * `getentropy` there.
 *
 * @see "The platform backend", in the guide.
 */
class sealer : public backend::sealer, public std::enable_shared_from_this<sealer> {
public:
    /** What a seal or an open completes with: an error, or the text, a view valid for the call. */
    using done_handler = handler<void(backend::error_code, std::string_view)>;

    /**
     * Constructs a sealer under a room's key, which it imports on its first operation.
     *
     * @param ex The executor its completions run on, an io_context's of webcpp::asio, which must
     * outlive the sealer's pending promises.
     * @param key The raw AES-256 key, `protocol::seal_key`'s.
     * @param memory The room's resource, which its texts, its waiting operations and its
     * completions are allocated in.
     */
    sealer(webcpp::asio::any_io_executor ex, std::array<std::uint8_t, 32> key,
           std::pmr::memory_resource* memory)
        : ex_(std::move(ex)), raw_key_(key), memory_(memory), waiting_(memory) {}

    /**
     * Seals a text under a fresh random IV, with `crypto.subtle.encrypt`.
     *
     * @param plaintext The text, read during the call.
     * @param done Called with the sealed text, or with `error::backend_failure` when the key
     * cannot be imported, the page has no entropy for the IV, or the encryption fails.
     */
    void seal(std::string_view plaintext, done_handler done) override {
        if (state_ == key_state::ready) {
            encrypt(plaintext, std::move(done));
            return;
        }
        wait_for_key({std::allocator_arg, allocator(),
                      [plaintext = std::pmr::string(plaintext, memory_), done = std::move(done)](
                          sealer& self, bool ok) mutable {
                          if (!ok) {
                              done(make_error_code(error::backend_failure), {});
                              return;
                          }
                          self.encrypt(plaintext, std::move(done));
                      }});
    }

    /**
     * Opens a sealed text, with `crypto.subtle.decrypt`.
     *
     * @param sealed The sealed text, read during the call.
     * @param done Called with the plaintext, or with `error::wrong_password` when the text was
     * sealed under another key or is not a sealed text, and `error::backend_failure` when the key
     * cannot be imported.
     */
    void open(std::string_view sealed, done_handler done) override {
        if (state_ == key_state::ready) {
            decrypt(sealed, std::move(done));
            return;
        }
        wait_for_key({std::allocator_arg, allocator(),
                      [sealed = std::pmr::string(sealed, memory_), done = std::move(done)](
                          sealer& self, bool ok) mutable {
                          if (!ok) {
                              done(make_error_code(error::backend_failure), {});
                              return;
                          }
                          self.decrypt(sealed, std::move(done));
                      }});
    }

private:
    using waiting_operation = handler<void(sealer&, bool)>;

    [[nodiscard]] std::pmr::polymorphic_allocator<> allocator() const { return {memory_}; }

    /** Calls `done` with `failure` after the call, on the executor. */
    void fail(done_handler done, trystero::error failure) {
        trystero::detail::post(ex_, memory_, [done = std::move(done), failure]() mutable {
            done(make_error_code(failure), {});
        });
    }

    /**
     * Awaits `promise`, its settlement posted to `handler` through the io_context's own executor
     * when `ex_` is one.
     *
     * Tip: as `trystero::detail::post` does. Asio posts what it posts through an `any_io_executor`
     * in a function object and an operation of `std::allocator`, which it takes from the C library
     * when its cache has none (measured: two for each seal and each open); through the io_context's
     * own executor the operation is allocated as the handler's allocator says, in the room's
     * resource.
     */
    template <class Handler>
    void await(emscripten::val promise, Handler&& handler) {
        if (const auto* io = ex_.target<webcpp::asio::io_context::executor_type>()) {
            webcpp::asio::browser::async_await(*io, std::move(promise),
                                               std::forward<Handler>(handler));
            return;
        }
        webcpp::asio::browser::async_await(ex_, std::move(promise), std::forward<Handler>(handler));
    }

    void encrypt(std::string_view plaintext, done_handler done) {
        std::array<std::uint8_t, protocol::seal_iv_size> iv{};
        if (!detail::fill_random(iv)) {
            fail(std::move(done), error::backend_failure);
            return;
        }
        await(detail::take(detail::js_crypt(false, key_.as_handle(), iv.data(), iv.size(),
                                            plaintext.data(), plaintext.size())),
              webcpp::asio::bind_allocator(
                  allocator(),
                  [iv, done = std::move(done), memory = memory_](
                      boost::system::error_code ec, const emscripten::val& cipher) mutable {
                      if (ec) {
                          done(make_error_code(error::backend_failure), {});
                          return;
                      }
                      const auto sealed =
                          protocol::format_sealed(iv, detail::from_js(cipher, memory), memory);
                      done({}, sealed);
                  }));
    }

    void decrypt(std::string_view sealed, done_handler done) {
        const auto parts = protocol::parse_sealed(sealed, memory_);
        if (!parts || parts->iv.empty()) {
            fail(std::move(done), error::wrong_password);
            return;
        }
        await(detail::take(detail::js_crypt(true, key_.as_handle(), parts->iv.data(),
                                            parts->iv.size(), parts->ciphertext.data(),
                                            parts->ciphertext.size())),
              webcpp::asio::bind_allocator(allocator(), [done = std::move(done), memory = memory_](
                                                            boost::system::error_code ec,
                                                            const emscripten::val& plain) mutable {
                  if (ec) {
                      done(make_error_code(error::wrong_password), {});
                      return;
                  }
                  const auto b = detail::from_js(plain, memory);
                  done({}, std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
              }));
    }

    /**
     * Runs `op` once the key is imported, or has failed to be: after the call in every case, so
     * that no completion runs inside the operation that started it.
     */
    void wait_for_key(waiting_operation op) {
        const std::weak_ptr<sealer> weak = weak_from_this();
        if (state_ == key_state::failed) {
            trystero::detail::post(ex_, memory_, [weak, op = std::move(op)]() mutable {
                if (const auto self = weak.lock()) {
                    op(*self, false);
                }
            });
            return;
        }
        waiting_.push_back(std::move(op));
        if (state_ == key_state::importing) {
            return;
        }
        state_ = key_state::importing;
        await(detail::take(detail::js_import_key(raw_key_.data(), raw_key_.size())),
              webcpp::asio::bind_allocator(
                  allocator(), [weak](boost::system::error_code ec, const emscripten::val& key) {
                      if (const auto self = weak.lock()) {
                          self->key_imported(!ec, key);
                      }
                  }));
    }

    void key_imported(bool ok, const emscripten::val& key) {
        state_ = ok ? key_state::ready : key_state::failed;
        key_ = key;
        std::pmr::deque<waiting_operation> ready(allocator());
        ready.swap(waiting_);
        for (auto& op : ready) {
            op(*this, ok);
        }
    }

    enum class key_state : std::uint8_t { none, importing, ready, failed };

    webcpp::asio::any_io_executor ex_;
    std::array<std::uint8_t, 32> raw_key_;
    std::pmr::memory_resource* memory_;
    key_state state_ = key_state::none;
    emscripten::val key_ = emscripten::val::undefined();
    std::pmr::deque<waiting_operation> waiting_;
};

#else

/**
 * AES-GCM under one room key, in Trystero's sealed text form: natively through OpenSSL, what
 * Trystero's `encrypt` and `decrypt` do with `crypto.subtle`.
 *
 * It works synchronously and posts each completion to the executor, as the backend interface asks:
 * a handler never runs inside the call that started it. Its texts and completions are in the
 * room's resource; OpenSSL's cipher context is OpenSSL's. A seal's IV comes from the platform's one
 * source of randomness, OpenSSL's generator there. It needs no exceptions.
 *
 * @see "The platform backend", in the guide.
 */
class sealer : public backend::sealer {
public:
    /** What a seal or an open completes with: an error, or the text, a view valid for the call. */
    using done_handler = handler<void(backend::error_code, std::string_view)>;

    /**
     * Constructs a sealer under a room's key.
     *
     * @param ex The executor its completions are posted to.
     * @param key The raw AES-256 key, `protocol::seal_key`'s.
     * @param memory The room's resource, which its texts and its completions are allocated in; it
     * must outlive the completions it posts.
     */
    sealer(webcpp::asio::any_io_executor ex, std::array<std::uint8_t, 32> key,
           std::pmr::memory_resource* memory)
        : ex_(std::move(ex)), key_(key), memory_(memory) {}

    /**
     * Seals a text under a fresh random IV.
     *
     * @param plaintext The text, read during the call.
     * @param done Called on the executor with the sealed text, or with `error::backend_failure`
     * when there is no randomness for the IV or OpenSSL fails.
     */
    void seal(std::string_view plaintext, done_handler done) override {
        std::array<std::uint8_t, protocol::seal_iv_size> iv{};
        if (!detail::fill_random(iv)) {
            complete(std::move(done), error::backend_failure);
            return;
        }
        const auto cipher = detail::aes_gcm(
            true, key_, iv,
            std::span(reinterpret_cast<const std::uint8_t*>(plaintext.data()), plaintext.size()),
            memory_);
        if (!cipher) {
            complete(std::move(done), error::backend_failure);
            return;
        }
        complete(std::move(done), {}, protocol::format_sealed(iv, *cipher, memory_));
    }

    /**
     * Opens a sealed text.
     *
     * @param sealed The sealed text, read during the call.
     * @param done Called on the executor with the plaintext, or with `error::wrong_password` when
     * the text was sealed under another key or is not a sealed text.
     */
    void open(std::string_view sealed, done_handler done) override {
        const auto parts = protocol::parse_sealed(sealed, memory_);
        const auto plain = parts && !parts->iv.empty()
                               ? detail::aes_gcm(false, key_, parts->iv, parts->ciphertext, memory_)
                               : std::nullopt;
        if (!plain) {
            complete(std::move(done), error::wrong_password);
            return;
        }
        complete(std::move(done), {}, std::pmr::string(plain->begin(), plain->end(), memory_));
    }

private:
    void complete(done_handler done, backend::error_code ec, std::pmr::string text) {
        trystero::detail::post(
            ex_, memory_,
            [done = std::move(done), ec, text = std::move(text)]() mutable { done(ec, text); });
    }

    void complete(done_handler done, error e) {
        complete(std::move(done), make_error_code(e), std::pmr::string(memory_));
    }

    webcpp::asio::any_io_executor ex_;
    std::array<std::uint8_t, 32> key_;
    std::pmr::memory_resource* memory_;
};

#endif

}  // namespace webcpp::trystero::platform

#endif
