// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_NATIVE_SEALER_HPP
#define WEBCPP_TRYSTERO_NATIVE_SEALER_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/protocol/signal.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/post.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace webcpp::trystero::native {

namespace detail {

struct cipher_context_deleter {
    void operator()(EVP_CIPHER_CTX* c) const { EVP_CIPHER_CTX_free(c); }
};

using cipher_context = std::unique_ptr<EVP_CIPHER_CTX, cipher_context_deleter>;

inline constexpr std::size_t gcm_tag_size = 16;

/** AES-256-GCM with a 16-byte IV, the ciphertext followed by its 16-byte tag: what WebCrypto's encrypt returns. */
inline std::optional<backend::bytes> aes_gcm(bool encrypt, const std::array<std::uint8_t, 32>& key, std::span<const std::uint8_t> iv,
                                             std::span<const std::uint8_t> input) {
    if (!encrypt && input.size() < gcm_tag_size) return std::nullopt;
    const cipher_context context(EVP_CIPHER_CTX_new());
    if (!context) return std::nullopt;
    const auto body = encrypt ? input : input.first(input.size() - gcm_tag_size);
    backend::bytes out(body.size() + (encrypt ? gcm_tag_size : 0));
    int length = 0;
    int ok = EVP_CipherInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr, encrypt ? 1 : 0);
    ok = ok && EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr);
    ok = ok && EVP_CipherInit_ex(context.get(), nullptr, nullptr, key.data(), iv.data(), -1);
    ok = ok && EVP_CipherUpdate(context.get(), out.data(), &length, body.data(), static_cast<int>(body.size()));
    if (!encrypt) {
        auto* tag = const_cast<std::uint8_t*>(input.data() + body.size());
        ok = ok && EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(gcm_tag_size), tag);
    }
    int final_length = 0;
    ok = ok && EVP_CipherFinal_ex(context.get(), out.data() + length, &final_length);
    if (encrypt) ok = ok && EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(gcm_tag_size), out.data() + body.size());
    if (!ok) return std::nullopt;
    return out;
}

}  // namespace detail

/**
 * AES-GCM under one room key, through OpenSSL, in Trystero's sealed text form. It works synchronously and posts each
 * completion to the executor, as the backend's contract asks: a handler never runs inside the call that started it.
 */
class sealer : public backend::sealer {
public:
    sealer(boost::asio::any_io_executor ex, std::array<std::uint8_t, 32> key) : ex_(std::move(ex)), key_(key) {}

    void seal(std::string_view plaintext, std::function<void(backend::error_code, std::string_view)> done) override {
        std::array<std::uint8_t, protocol::seal_iv_size> iv{};
        if (RAND_bytes(iv.data(), static_cast<int>(iv.size())) != 1) return complete(std::move(done), error::backend_failure, {});
        const auto cipher = detail::aes_gcm(true, key_, iv, std::span(reinterpret_cast<const std::uint8_t*>(plaintext.data()), plaintext.size()));
        if (!cipher) return complete(std::move(done), error::backend_failure, {});
        complete(std::move(done), {}, protocol::format_sealed(iv, *cipher));
    }

    void open(std::string_view sealed, std::function<void(backend::error_code, std::string_view)> done) override {
        const auto parts = protocol::parse_sealed(sealed);
        const auto plain = parts && !parts->iv.empty() ? detail::aes_gcm(false, key_, parts->iv, parts->ciphertext) : std::nullopt;
        if (!plain) return complete(std::move(done), error::wrong_password, {});
        complete(std::move(done), {}, std::string(plain->begin(), plain->end()));
    }

private:
    void complete(std::function<void(backend::error_code, std::string_view)> done, backend::error_code ec, std::string text) {
        boost::asio::post(ex_, [done = std::move(done), ec, text = std::move(text)] { done(ec, text); });
    }

    void complete(std::function<void(backend::error_code, std::string_view)> done, error e, std::string text) {
        complete(std::move(done), make_error_code(e), std::move(text));
    }

    boost::asio::any_io_executor ex_;
    std::array<std::uint8_t, 32> key_;
};

}  // namespace webcpp::trystero::native

#endif
