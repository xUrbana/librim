#pragma once

#include <cstring>
#include <functional>
#include <span>
#include <unordered_map>

namespace librim
{

/**
 * @class MessageDispatcher
 * @brief Zero-allocation router mapping raw byte streams to concrete application logic.
 *
 * `MessageDispatcher` provides a strongly-typed routing schema. Incoming generic bytes 
 * natively trigger explicitly mapped structs, eliminating immense amounts of manual 
 * `memcpy` and `switch` statements across network handlers.
 *
 * @tparam MessageType The enumerator or integer type defining the unique message layout constraint.
 * @tparam HeaderType The static prefix structure physically bridging all packets.
 */
template <typename MessageType, typename HeaderType> class MessageDispatcher
{
  public:
    /**
     * @brief Logical output returning extracted dimensions needed to parse subsequent bytes.
     */
    using ExtractorResult = std::pair<MessageType, std::size_t>;

    /**
     * @brief Custom evaluator mapping raw header blocks into explicit bounds.
     */
    using HeaderExtractor = std::function<ExtractorResult(const HeaderType &)>;

    /**
     * @brief Underlying representation mapping type-punned functions securely.
     */
    using RawHandler = std::function<void(std::span<const std::byte>)>;

    /**
     * @brief Initializes the underlying routing table mechanism.
     * @param extractor Logical lambda deducing concrete constraints out of generic headers.
     */
    MessageDispatcher(HeaderExtractor extractor) : extractor_(std::move(extractor))
    {
    }

    /**
     * @brief Binds a callable lambda hooking dynamic network arrivals directly to concrete typed structs.
     * 
     * @tparam MsgDef Statically-sized physical C++ struct representing the active transmission.
     * @tparam Callable Function object taking precisely one `(MsgDef&)` parameter.
     * @param type Unique numeric identifier correlating the `MsgDef` to its network ID.
     * @param handler The lambda encapsulating active business logic.
     */
    template <typename MsgDef, typename Callable> 
    void add_handler(MessageType type, Callable handler)
    {
        handlers_[type] = [h = std::move(handler)](std::span<const std::byte> data) {
            // Drop aggressively malformed/truncated packets preventing bounds-read vulnerabilities
            if (data.size() < sizeof(MsgDef))
                return;
                
            // Natively type-pun the generic span array directly into local struct fields
            MsgDef *ptr = reinterpret_cast<MsgDef *>(const_cast<std::byte *>(data.data()));
            h(*ptr);
        };
    }

    /**
     * @brief Binds a static C-style function pointer directly to a payload layout.
     * 
     * @tparam MsgDef Struct layout type deduced directly via the function signature.
     * @param type The unique numeric identity of the packet.
     * @param handler The target unbound function execution target.
     */
    template <typename MsgDef> 
    void add_handler(MessageType type, void (*handler)(MsgDef &))
    {
        handlers_[type] = [handler](std::span<const std::byte> data) {
            if (data.size() < sizeof(MsgDef))
                return; // Drop explicitly
            MsgDef *ptr = reinterpret_cast<MsgDef *>(const_cast<std::byte *>(data.data()));
            handler(*ptr);
        };
    }

    /**
     * @brief Subordinates a message reception directly to a member function of a specific active Class instance.
     * 
     * @tparam MsgDef Type signature of the packet.
     * @tparam ClassType Deduced surrounding object ownership natively executing the logic.
     * @param type Unique layout ID.
     * @param instance The "this" pointer corresponding broadly to the target.
     * @param handler Pointer-To-Member function safely invoked against the object context.
     */
    template <typename MsgDef, typename ClassType>
    void add_handler(MessageType type, ClassType *instance, void (ClassType::*handler)(MsgDef &))
    {
        handlers_[type] = [instance, handler](std::span<const std::byte> data) {
            if (data.size() < sizeof(MsgDef))
                return;
            MsgDef *ptr = reinterpret_cast<MsgDef *>(const_cast<std::byte *>(data.data()));
            
            // Re-bind the object context invoking the explicit handler
            (instance->*handler)(*ptr);
        };
    }

    /**
     * @brief Primary ingestion point actively evaluating stream data and feeding the correct configured handler.
     * 
     * @param data Full contiguous representation encompassing both Header + Subordinate Payload arrays.
     * @return `true` if uniquely handled, `false` if unregistered or logically malformed.
     */
    bool dispatch(std::span<const std::byte> data) const
    {
        // Impossible bounds check
        if (data.size() < sizeof(HeaderType))
        {
            return false;
        }

        // Extremely fast stack copy
        HeaderType header;
        std::memcpy(&header, data.data(), sizeof(HeaderType));

        auto [type, size] = extractor_(header);
        
        // Native unordered_map hash lookup
        auto it = handlers_.find(type);
        if (it != handlers_.end())
        {
            it->second(data);
            return true;
        }
        return false;
    }

    /**
     * @brief Transmutes the Dispatcher's static prefix logic out into the ASIO TCP Client pipelines.
     * 
     * @return Valid executable parsing lambda translating Headers to Length explicitly.
     */
    std::function<std::size_t(std::span<const std::byte>)> get_size_lambda() const
    {
        return [ext = extractor_](std::span<const std::byte> data) {
            HeaderType header;
            std::memcpy(&header, data.data(), sizeof(HeaderType));
            return ext(header).second;
        };
    }

    /**
     * @brief Accessor retrieving the immutable size representing the static prefix chunk.
     * @return Bytes representing framing dimension logic.
     */
    std::size_t header_size() const
    {
        return sizeof(HeaderType);
    }

  private:
    HeaderExtractor extractor_; ///< Internal logic generating message bounds.
    std::unordered_map<MessageType, RawHandler> handlers_; ///< Hash-based ultra-fast dynamic routing pipeline.
};

} // namespace librim
