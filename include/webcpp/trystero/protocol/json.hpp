// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PROTOCOL_JSON_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_JSON_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/document/document.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/parser.hpp>
#include <webcpp/document/quote.hpp>
#include <webcpp/document/stringify.hpp>
#include <webcpp/document/traits.hpp>
#include <webcpp/document/value.hpp>

#include <boost/assert.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

/**
 * Reading and writing the protocol's JSON texts on webcpp::document, with every value in the
 * resource the caller gives. A text is read into a document whose image is a buffer of that
 * resource; a text the protocol writes is a fixed shape of a few strings and numbers, so it is
 * appended field by field, its strings quoted as `JSON.stringify` quotes them, rather than built as
 * a document, which would copy every string into a node of its own before writing it again.
 *
 * Tip: document's parser and writer keep their working state (Boost.JSON's parser stack, the path
 * and the pending text, the tree's record of each change, the writer's stack) in the standard
 * allocator, each freed before the call that made it returns: the guide's "Allocation" lists it
 * outside the resource, measured.
 */
namespace webcpp::trystero::protocol::json {

/** The traits of every document trystero reads and writes: document's defaults, UTF-8. */
using traits = webcpp::document::default_traits;

/** The read-only handle of a value of a @ref document. */
using const_value = webcpp::document::basic_const_value<traits, webcpp::document::external>;

/** The handle of a value of a @ref document, which reads and writes. */
using value = webcpp::document::basic_value<traits, webcpp::document::external>;

class document;

webcpp::document::result<document> parse(std::string_view text,
                                         const webcpp::document::parse_options& options,
                                         std::pmr::memory_resource* memory);

/**
 * A JSON document whose image is a buffer of a memory resource: every value in it, strings
 * included, lives in that buffer, which the document owns and gives back when it goes.
 *
 * The buffer never grows: a change that needs more room returns `errc::capacity`, as document's
 * external storage does. @ref parse sizes it for the text it reads. It is move-only; moving keeps
 * the buffer where it is, so the handles of its values stay valid.
 *
 * @see "Allocation", in the guide.
 */
class document {
public:
    /** The document over the buffer: document's external storage. */
    using document_type = webcpp::document::basic_document<traits, webcpp::document::external>;

    /** A document that holds no image, as one moved from does. */
    document() noexcept = default;

    /**
     * Takes the image and the buffer of `other`, which holds nothing after it.
     *
     * @param other The document.
     */
    document(document&& other) noexcept
        : memory_(std::exchange(other.memory_, nullptr)),
          buffer_(std::exchange(other.buffer_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          document_(std::move(other.document_)) {
        other.document_.reset();
    }

    /**
     * Gives this document's buffer back, then takes the image and the buffer of `other`.
     *
     * @param other The document.
     * @return This document.
     */
    document& operator=(document&& other) noexcept {
        if (this != &other) {
            release();
            memory_ = std::exchange(other.memory_, nullptr);
            buffer_ = std::exchange(other.buffer_, nullptr);
            size_ = std::exchange(other.size_, 0);
            document_ = std::move(other.document_);
            other.document_.reset();
        }
        return *this;
    }

    /** A document is not copied: `get().clone()` copies its content into an owned document. */
    document(const document&) = delete;

    /** A document is not copied: `get().clone()` copies its content into an owned document. */
    document& operator=(const document&) = delete;

    /** Gives the buffer back to its resource. */
    ~document() { release(); }

    /**
     * Makes a new document holding `null`, in a buffer of `bytes` from `memory`.
     *
     * @param bytes The size of the buffer, which every change of the document must fit; rounded up
     * to 16.
     * @param memory The resource the buffer comes from; it must outlive the document.
     * @return The document; `errc::capacity` when `bytes` cannot hold an empty document.
     */
    static webcpp::document::result<document> create(std::size_t bytes,
                                                     std::pmr::memory_resource* memory) {
        document made(memory, bytes);
        auto formatted = webcpp::document::format<traits>(made.buffer());
        if (!formatted.has_value()) {
            return formatted.error();
        }
        made.document_.emplace(std::move(formatted).unsafe_value());
        return made;
    }

    /**
     * Whether it holds an image: false for one default-constructed or moved from.
     *
     * @return Whether there is a value to read.
     */
    [[nodiscard]] bool has_value() const noexcept { return document_.has_value(); }

    /**
     * The handle of the document's value.
     *
     * @return The handle; one that names nothing, which reads `undefined`, when it holds no image.
     */
    [[nodiscard]] const_value root() const noexcept {
        return document_.has_value() ? std::as_const(*document_).root() : const_value();
    }

    /**
     * The handle of the document's value, which writes in the buffer.
     *
     * @return The handle; one that names nothing when it holds no image.
     */
    [[nodiscard]] value root() noexcept {
        return document_.has_value() ? document_->root() : value();
    }

    /**
     * document's own document over the buffer, for what the handles do not do, such as `clone`
     * and `image`.
     *
     * @return The document; it must hold an image.
     */
    [[nodiscard]] const document_type& get() const noexcept {
        BOOST_ASSERT(document_.has_value());
        return *document_;  // NOLINT(bugprone-unchecked-optional-access): asserted
    }

    /**
     * document's own document over the buffer, which writes.
     *
     * @return The document; it must hold an image.
     */
    [[nodiscard]] document_type& get() noexcept {
        BOOST_ASSERT(document_.has_value());
        return *document_;  // NOLINT(bugprone-unchecked-optional-access): asserted
    }

    /**
     * The resource the buffer comes from.
     *
     * @return The resource; null when it holds no image.
     */
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept { return memory_; }

    /**
     * The size of the buffer, the most the image can take.
     *
     * @return The bytes; 0 when it holds no image.
     */
    [[nodiscard]] std::size_t capacity() const noexcept { return size_; }

private:
    friend webcpp::document::result<document> parse(std::string_view text,
                                                    const webcpp::document::parse_options& options,
                                                    std::pmr::memory_resource* memory);

    /** The alignment of document's images. */
    static constexpr std::size_t alignment = 16;

    /** `bytes` rounded up to the alignment. */
    static constexpr std::size_t aligned(std::size_t bytes) noexcept {
        return (bytes + alignment - 1) / alignment * alignment;
    }

    /** A document with a buffer of `bytes`, rounded up to 16, from `memory`, and no image yet. */
    document(std::pmr::memory_resource* memory, std::size_t bytes)
        : memory_(memory),
          buffer_(static_cast<std::byte*>(memory->allocate(aligned(bytes), alignment))),
          size_(aligned(bytes)) {}

    /** The buffer. */
    [[nodiscard]] std::span<std::byte> buffer() const noexcept { return {buffer_, size_}; }

    /** Ends the document, then gives its buffer back. */
    void release() noexcept {
        document_.reset();
        if (buffer_ != nullptr) {
            memory_->deallocate(buffer_, size_, alignment);
        }
        memory_ = nullptr;
        buffer_ = nullptr;
        size_ = 0;
    }

    std::pmr::memory_resource* memory_ = nullptr;
    std::byte* buffer_ = nullptr;
    std::size_t size_ = 0;
    std::optional<document_type> document_;
};

/**
 * The limits of a protocol text's parse: at most `max_bytes` of text, into an image at most twice
 * that and 64 KiB more.
 *
 * Tip: a text Trystero writes holds a few dozen values at most, each node some 200 bytes of image
 * beside its string, so the image of the longest is under its text and 16 KiB. A text that needs
 * more holds thousands of values, which no Trystero message has; it is refused once its parse has
 * filled the bound, so that no text costs a room more than a multiple of its bound to read.
 *
 * @param max_bytes The longest text the caller reads.
 * @return The options.
 */
constexpr webcpp::document::parse_options protocol_limits(std::uint64_t max_bytes) noexcept {
    webcpp::document::parse_options o;
    o.max_bytes = max_bytes;
    o.max_document_bytes = (2 * max_bytes) + (std::uint64_t{64} << 10U);
    return o;
}

/**
 * `JSON.parse(text)` into a @ref document in a buffer of `memory`, as webcpp::document reads it: a
 * key written twice keeps the last value in the first one's place, and a lone surrogate is
 * refused.
 *
 * The buffer is first sized for a text of a few values, twice the text and 4 KiB; a parse that
 * fills it is made again in a buffer twice the size, up to `options.max_document_bytes`.
 *
 * @param text The text.
 * @param options The limits: `max_bytes` of text, checked before anything is read,
 * `max_document_bytes` of buffer, and `max_depth`.
 * @param memory The resource the buffer comes from; it must outlive the document.
 * @return The document; `errc::capacity` for a text longer than `max_bytes`, nested deeper than
 * `max_depth`, or whose image needs more than `max_document_bytes`, `errc::syntax` for malformed
 * text, `errc::range` for a number document refuses.
 */
inline webcpp::document::result<document> parse(std::string_view text,
                                                const webcpp::document::parse_options& options,
                                                std::pmr::memory_resource* memory) {
    if (text.size() > options.max_bytes) {
        return webcpp::document::errc::capacity;
    }
    constexpr std::size_t first_spare = std::size_t{4} << 10U;
    const auto most = static_cast<std::size_t>(std::min<std::uint64_t>(
        options.max_document_bytes, std::numeric_limits<std::size_t>::max() / 4));
    std::size_t bytes = std::min((2 * text.size()) + first_spare, most);
    for (;;) {
        document made(memory, bytes);
        webcpp::document::basic_parser<traits, webcpp::document::external> parser(made.buffer(),
                                                                                  options);
        const webcpp::document::result<void> written = parser.write(text);
        auto finished = written.has_value()
                            ? parser.finish()
                            : webcpp::document::result<document::document_type>(written.error());
        if (finished.has_value()) {
            made.document_.emplace(std::move(finished).unsafe_value());
            return made;
        }
        // A full buffer is errc::capacity without a cause of Boost.JSON's, which a text nested past
        // max_depth has.
        const bool full = finished.error() == webcpp::document::errc::capacity && !parser.cause();
        if (!full || bytes >= most) {
            return finished.error();
        }
        bytes = std::min(2 * bytes, most);
    }
}

/**
 * The text a value holds.
 *
 * @param v The value.
 * @return The text, valid while its document is unchanged; empty when it holds no string.
 */
inline std::string_view text_of(const const_value& v) noexcept {
    return v.value_or(std::string_view());
}

namespace detail {

/** An output of `webcpp::document::quote` that keeps nothing: what checks that a text quotes. */
struct discard {
    /** Takes a piece of the literal, and drops it. */
    void append(const char* /*data*/, std::size_t /*size*/) noexcept {}
};

}  // namespace detail

/**
 * Whether `text` can be written as a JSON string: whether it is UTF-8, as every JavaScript string
 * is once encoded.
 *
 * @param text The text.
 * @return Whether @ref append_string writes it.
 */
inline bool quotable(std::string_view text) {
    detail::discard nothing;
    return webcpp::document::quote(text, nothing).has_value();
}

/**
 * Appends `text` as a JSON string literal, quotes included, as `JSON.stringify` writes it, with
 * webcpp::document's quoting, which allocates only through `out`.
 *
 * @param out The text to append to.
 * @param text The string, in UTF-8 (@ref quotable): every string the protocol writes is, the self
 * id checked when the app is made, a candidate's fields when it is written, and every other one is
 * ASCII or was read from JSON.
 */
inline void append_string(std::pmr::string& out, std::string_view text) {
    [[maybe_unused]] const webcpp::document::result<void> quoted =
        webcpp::document::quote(text, out);
    BOOST_ASSERT(quoted.has_value());
}

/**
 * Appends a value as `JSON.stringify` writes it.
 *
 * @tparam Storage The storage of the value's document.
 * @param out The text to append to. The writer hands it pieces of at most 8 bytes, which no
 * standard library's string holds outside its own object, so that only `out` allocates the text.
 * @param v The value; `undefined` writes nothing.
 */
template <class Storage>
// A handle is taken by value, as document passes every handle.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
void append_value(std::pmr::string& out, webcpp::document::basic_const_value<traits, Storage> v) {
    constexpr std::uint64_t piece = 8;
    // A sink that returns nothing never fails, so neither does the writing.
    static_cast<void>(webcpp::document::stringify_to(
        v, [&out](std::string_view text) { out.append(text); }, 0, piece));
}

/**
 * Appends an integer in decimal, as JavaScript writes a safe integer.
 *
 * @param out The text to append to.
 * @param value The integer.
 */
inline void append_integer(std::pmr::string& out, std::int64_t value) {
    std::array<char, 24> buffer{};
    const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    out.append(buffer.data(), end);
}

/**
 * Appends `"key":` with a leading comma unless it opens the object.
 *
 * @param out The text of an object being written, which ends with its `{` or a member's value.
 * @param key The member's name.
 */
inline void append_key(std::pmr::string& out, std::string_view key) {
    if (out.back() != '{') {
        out += ',';
    }
    append_string(out, key);
    out += ':';
}

}  // namespace webcpp::trystero::protocol::json

#endif
