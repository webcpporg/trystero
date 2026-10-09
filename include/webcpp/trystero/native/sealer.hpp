// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_NATIVE_SEALER_HPP
#define WEBCPP_TRYSTERO_NATIVE_SEALER_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/post.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/protocol/signal.hpp>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <boost/asio/any_io_executor.hpp>  // lint-world: posts handlers only
#include <boost/asio/post.hpp>             // lint-world: posts handlers only

#include <array>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace webcpp::trystero::native {

namespace detail {

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

}  // namespace detail

/**
 * AES-GCM under one room key, through OpenSSL, in Trystero's sealed text form, what Trystero's
 * `encrypt` and `decrypt` do with `crypto.subtle`.
 *
 * It works synchronously and posts each completion to the executor, as the backend interface asks:
 * a handler never runs inside the call that started it. Its texts and completions are in the
 * room's resource; OpenSSL's cipher context is OpenSSL's. It needs no exceptions.
 *
 * @see "The native backend", in the guide.
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
    sealer(boost::asio::any_io_executor ex, std::array<std::uint8_t, 32> key,
           std::pmr::memory_resource* memory)
        : ex_(std::move(ex)), key_(key), memory_(memory) {}

    /**
     * Seals a text under a fresh IV from OpenSSL's random generator.
     *
     * @param plaintext The text, read during the call.
     * @param done Called on the executor with the sealed text, or with `error::backend_failure`
     * when OpenSSL fails.
     */
    void seal(std::string_view plaintext, done_handler done) override {
        std::array<std::uint8_t, protocol::seal_iv_size> iv{};
        if (RAND_bytes(iv.data(), static_cast<int>(iv.size())) != 1) {  // lint-world: a seal's IV
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

    boost::asio::any_io_executor ex_;
    std::array<std::uint8_t, 32> key_;
    std::pmr::memory_resource* memory_;
};

}  // namespace webcpp::trystero::native

#endif
