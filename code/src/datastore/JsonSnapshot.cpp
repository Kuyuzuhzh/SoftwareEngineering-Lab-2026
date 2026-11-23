#include "JsonSnapshot.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>
#include <variant>

namespace ledger {
namespace {

struct Json {
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;
    std::variant<std::nullptr_t, bool, std::int64_t, std::string, Array, Object> value;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    bool Parse(Json& json) {
        SkipWhitespace();
        if (!ParseValue(json, 0)) {
            return false;
        }
        SkipWhitespace();
        return position_ == text_.size();
    }

private:
    void SkipWhitespace() {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\n' ||
                text_[position_] == '\r' || text_[position_] == '\t')) {
            ++position_;
        }
    }

    bool Consume(char expected) {
        SkipWhitespace();
        if (position_ >= text_.size() || text_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    bool Literal(std::string_view value) {
        if (text_.substr(position_, value.size()) != value) {
            return false;
        }
        position_ += value.size();
        return true;
    }

    bool ParseValue(Json& json, int depth) {
        if (depth > 64) {
            return false;
        }
        SkipWhitespace();
        if (position_ >= text_.size()) {
            return false;
        }
        switch (text_[position_]) {
        case 'n':
            if (!Literal("null")) return false;
            json.value = nullptr;
            return true;
        case 't':
            if (!Literal("true")) return false;
            json.value = true;
            return true;
        case 'f':
            if (!Literal("false")) return false;
            json.value = false;
            return true;
        case '"': {
            std::string value;
            if (!ParseString(value)) return false;
            json.value = std::move(value);
            return true;
        }
        case '[':
            return ParseArray(json, depth + 1);
        case '{':
            return ParseObject(json, depth + 1);
        default:
            return ParseNumber(json);
        }
    }

    bool ParseArray(Json& json, int depth) {
        if (!Consume('[')) return false;
        Json::Array array;
        SkipWhitespace();
        if (position_ < text_.size() && text_[position_] == ']') {
            ++position_;
            json.value = std::move(array);
            return true;
        }
        while (true) {
            Json element;
            if (!ParseValue(element, depth)) return false;
            array.push_back(std::move(element));
            SkipWhitespace();
            if (position_ >= text_.size()) return false;
            if (text_[position_] == ']') {
                ++position_;
                json.value = std::move(array);
                return true;
            }
            if (text_[position_++] != ',') return false;
        }
    }

    bool ParseObject(Json& json, int depth) {
        if (!Consume('{')) return false;
        Json::Object object;
        SkipWhitespace();
        if (position_ < text_.size() && text_[position_] == '}') {
            ++position_;
            json.value = std::move(object);
            return true;
        }
        while (true) {
            SkipWhitespace();
            std::string key;
            if (!ParseString(key) || !Consume(':')) return false;
            Json value;
            if (!ParseValue(value, depth)) return false;
            if (!object.emplace(std::move(key), std::move(value)).second) return false;
            SkipWhitespace();
            if (position_ >= text_.size()) return false;
            if (text_[position_] == '}') {
                ++position_;
                json.value = std::move(object);
                return true;
            }
            if (text_[position_++] != ',') return false;
        }
    }

    static int Hex(char character) {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        return -1;
    }

    bool CodeUnit(std::uint32_t& value) {
        if (position_ + 4 > text_.size()) return false;
        value = 0;
        for (int index = 0; index < 4; ++index) {
            const int digit = Hex(text_[position_++]);
            if (digit < 0) return false;
            value = value * 16 + static_cast<std::uint32_t>(digit);
        }
        return true;
    }

    static void AppendUtf8(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7f) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0xffff) {
            output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    }

    bool ParseString(std::string& output) {
        if (position_ >= text_.size() || text_[position_++] != '"') return false;
        output.clear();
        while (position_ < text_.size()) {
            const auto character = static_cast<unsigned char>(text_[position_++]);
            if (character == '"') return true;
            if (character < 0x20) return false;
            if (character != '\\') {
                output.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ >= text_.size()) return false;
            switch (text_[position_++]) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                std::uint32_t codepoint{};
                if (!CodeUnit(codepoint)) return false;
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    if (position_ + 2 > text_.size() || text_[position_] != '\\' ||
                        text_[position_ + 1] != 'u') {
                        return false;
                    }
                    position_ += 2;
                    std::uint32_t low{};
                    if (!CodeUnit(low) || low < 0xdc00 || low > 0xdfff) return false;
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                    return false;
                }
                AppendUtf8(output, codepoint);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool ParseNumber(Json& json) {
        const auto start = position_;
        if (position_ < text_.size() && text_[position_] == '-') ++position_;
        if (position_ >= text_.size()) return false;
        if (text_[position_] == '0') {
            ++position_;
        } else {
            if (text_[position_] < '1' || text_[position_] > '9') return false;
            while (position_ < text_.size() && text_[position_] >= '0' &&
                   text_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < text_.size() &&
            (text_[position_] == '.' || text_[position_] == 'e' ||
             text_[position_] == 'E')) {
            return false;
        }
        std::int64_t value{};
        const auto parsed = std::from_chars(
            text_.data() + start,
            text_.data() + position_,
            value);
        if (parsed.ec != std::errc{} || parsed.ptr != text_.data() + position_) {
            return false;
        }
        json.value = value;
        return true;
    }

    std::string_view text_;
    std::size_t position_{0};
};

bool ToUtf8(const std::wstring& input, std::string& output) {
    output.clear();
    if (input.empty()) return true;
    const int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        input.data(),
        static_cast<int>(input.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) return false;
    output.resize(static_cast<std::size_t>(size));
    return WideCharToMultiByte(
               CP_UTF8,
               WC_ERR_INVALID_CHARS,
               input.data(),
               static_cast<int>(input.size()),
               output.data(),
               size,
               nullptr,
               nullptr) == size;
}

bool FromUtf8(const std::string& input, std::wstring& output) {
    output.clear();
    if (input.empty()) return true;
    const int size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        input.data(),
        static_cast<int>(input.size()),
        nullptr,
        0);
    if (size <= 0) return false;
    output.resize(static_cast<std::size_t>(size));
    return MultiByteToWideChar(
               CP_UTF8,
               MB_ERR_INVALID_CHARS,
               input.data(),
               static_cast<int>(input.size()),
               output.data(),
               size) == size;
}

void Quote(const std::string& value, std::string& output) {
    static constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20) {
                output += "\\u00";
                output.push_back(hex[character >> 4]);
                output.push_back(hex[character & 0x0f]);
            } else {
                output.push_back(static_cast<char>(character));
            }
        }
    }
    output.push_back('"');
}

void Quote(const std::wstring& value, std::string& output) {
    std::string utf8;
    if (!ToUtf8(value, utf8)) throw std::runtime_error("Invalid UTF-16 string.");
    Quote(utf8, output);
}

std::string FormatTime(TimePoint value) {
    const auto time = std::chrono::system_clock::to_time_t(value);
    std::tm utc{};
    if (gmtime_s(&utc, &time) != 0) throw std::runtime_error("Invalid timestamp.");
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

bool ParseTime(const std::string& text, TimePoint& value) {
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':' || text[19] != 'Z') {
        return false;
    }
    std::tm utc{};
    std::istringstream input{text};
    input >> std::get_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    if (input.fail() || input.peek() != std::char_traits<char>::eof()) return false;
    const auto time = _mkgmtime64(&utc);
    if (time == -1) return false;
    value = std::chrono::system_clock::from_time_t(static_cast<time_t>(time));
    return true;
}

const char* RoleName(Role role) {
    switch (role) {
    case Role::Creator: return "Creator";
    case Role::Auditor: return "Auditor";
    case Role::Member: return "Member";
    }
    throw std::runtime_error("Invalid role.");
}

const char* StateName(EntryState state) {
    switch (state) {
    case EntryState::Draft: return "Draft";
    case EntryState::Waiting_review: return "Waiting_review";
    case EntryState::Rejected: return "Rejected";
    case EntryState::Accepted: return "Accepted";
    case EntryState::Deleted: return "Deleted";
    }
    throw std::runtime_error("Invalid entry state.");
}

const char* TypeName(EntryType type) {
    switch (type) {
    case EntryType::Income: return "Income";
    case EntryType::expenditure: return "expenditure";
    }
    throw std::runtime_error("Invalid entry type.");
}

void WriteUser(const User& user, std::string& out) {
    out += "{\"id\":" + std::to_string(user.id) + ",\"username\":";
    Quote(user.username, out);
    out += ",\"display_name\":";
    Quote(user.display_name, out);
    out += ",\"password_hash\":";
    Quote(user.password_hash, out);
    out += "}";
}

void WriteBook(const Book& book, std::string& out) {
    out += "{\"id\":" + std::to_string(book.id) + ",\"name\":";
    Quote(book.name, out);
    out += ",\"creator_id\":" + std::to_string(book.creator_id) + ",\"members\":[";
    for (std::size_t index = 0; index < book.members.size(); ++index) {
        if (index != 0) out += ",";
        const auto& member = book.members[index];
        out += "{\"user_id\":" + std::to_string(member.user_id) + ",\"roles\":[";
        for (std::size_t role = 0; role < member.roles.size(); ++role) {
            if (role != 0) out += ",";
            Quote(RoleName(member.roles[role]), out);
        }
        out += "]}";
    }
    out += "],\"budget\":";
    if (book.budget) {
        out += "{\"cents\":" + std::to_string(book.budget->cents) + "}";
    } else {
        out += "null";
    }
    out += ",\"closed\":";
    out += book.closed ? "true}" : "false}";
}

void WriteOptionalTime(const std::optional<TimePoint>& value, std::string& out) {
    if (value) Quote(FormatTime(*value), out);
    else out += "null";
}

void WriteEntry(const Entry& entry, std::string& out) {
    out += "{\"id\":" + std::to_string(entry.id) +
           ",\"book_id\":" + std::to_string(entry.book_id) +
           ",\"submitter_id\":" + std::to_string(entry.submitter_id) +
           ",\"state\":";
    Quote(StateName(entry.state), out);
    out += ",\"type\":";
    Quote(TypeName(entry.type), out);
    out += ",\"amount_cents\":" + std::to_string(entry.amount.cents) +
           ",\"occurred_at\":";
    Quote(FormatTime(entry.occurred_at), out);
    out += ",\"payer\":";
    Quote(entry.payer, out);
    out += ",\"note\":";
    Quote(entry.note, out);
    out += ",\"attachments\":[";
    for (std::size_t index = 0; index < entry.attachments.size(); ++index) {
        if (index != 0) out += ",";
        Quote(entry.attachments[index].generic_wstring(), out);
    }
    out += "],\"created_at\":";
    Quote(FormatTime(entry.created_at), out);
    out += ",\"submitted_at\":";
    WriteOptionalTime(entry.submitted_at, out);
    out += ",\"reviewer_id\":";
    if (entry.reviewer_id) out += std::to_string(*entry.reviewer_id);
    else out += "null";
    out += ",\"reviewed_at\":";
    WriteOptionalTime(entry.reviewed_at, out);
    out += ",\"replaces_entry_id\":";
    if (entry.replaces_entry_id) out += std::to_string(*entry.replaces_entry_id);
    else out += "null";
    out += "}";
}

void WriteLog(const OperationLog& log, std::string& out) {
    out += "{\"id\":" + std::to_string(log.id) +
           ",\"book_id\":" + std::to_string(log.book_id) + ",\"entry_id\":";
    if (log.entry_id) out += std::to_string(*log.entry_id);
    else out += "null";
    out += ",\"actor_id\":" + std::to_string(log.actor_id) + ",\"action\":";
    Quote(log.action, out);
    out += ",\"operated_at\":";
    Quote(FormatTime(log.operated_at), out);
    out += ",\"before_json\":";
    Quote(log.before_json, out);
    out += ",\"after_json\":";
    Quote(log.after_json, out);
    out += ",\"device_info\":";
    Quote(log.device_info, out);
    out += "}";
}

const Json* Field(const Json::Object& object, const char* key) {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

bool ReadInt(const Json::Object& object, const char* key, std::int64_t& value) {
    const auto* json = Field(object, key);
    const auto* number = json ? std::get_if<std::int64_t>(&json->value) : nullptr;
    if (!number) return false;
    value = *number;
    return true;
}

bool ReadBool(const Json::Object& object, const char* key, bool& value) {
    const auto* json = Field(object, key);
    const auto* boolean = json ? std::get_if<bool>(&json->value) : nullptr;
    if (!boolean) return false;
    value = *boolean;
    return true;
}

bool ReadString(const Json::Object& object, const char* key, std::string& value) {
    const auto* json = Field(object, key);
    const auto* string = json ? std::get_if<std::string>(&json->value) : nullptr;
    if (!string) return false;
    value = *string;
    return true;
}

bool ReadWide(const Json::Object& object, const char* key, std::wstring& value) {
    std::string utf8;
    return ReadString(object, key, utf8) && FromUtf8(utf8, value);
}

bool ReadTime(const Json::Object& object, const char* key, TimePoint& value) {
    std::string text;
    return ReadString(object, key, text) && ParseTime(text, value);
}

bool ReadOptionalTime(
    const Json::Object& object,
    const char* key,
    std::optional<TimePoint>& value) {
    const auto* json = Field(object, key);
    if (!json) return false;
    if (std::holds_alternative<std::nullptr_t>(json->value)) {
        value.reset();
        return true;
    }
    const auto* text = std::get_if<std::string>(&json->value);
    TimePoint parsed{};
    if (!text || !ParseTime(*text, parsed)) return false;
    value = parsed;
    return true;
}

const Json::Array* ReadArray(const Json::Object& object, const char* key) {
    const auto* json = Field(object, key);
    return json ? std::get_if<Json::Array>(&json->value) : nullptr;
}

const Json::Object* AsObject(const Json& json) {
    return std::get_if<Json::Object>(&json.value);
}

bool ReadUser(const Json& json, User& user) {
    const auto* object = AsObject(json);
    return object && ReadInt(*object, "id", user.id) &&
           ReadWide(*object, "username", user.username) &&
           ReadWide(*object, "display_name", user.display_name) &&
           ReadString(*object, "password_hash", user.password_hash);
}

bool ReadBook(const Json& json, Book& book) {
    const auto* object = AsObject(json);
    if (!object || !ReadInt(*object, "id", book.id) ||
        !ReadWide(*object, "name", book.name) ||
        !ReadInt(*object, "creator_id", book.creator_id) ||
        !ReadBool(*object, "closed", book.closed)) {
        return false;
    }
    const auto* members = ReadArray(*object, "members");
    if (!members) return false;
    for (const auto& item : *members) {
        const auto* member = AsObject(item);
        const auto* roles = member ? ReadArray(*member, "roles") : nullptr;
        Membership value;
        if (!member || !roles || !ReadInt(*member, "user_id", value.user_id)) return false;
        for (const auto& role_json : *roles) {
            const auto* role = std::get_if<std::string>(&role_json.value);
            if (!role) return false;
            if (*role == "Creator") value.roles.push_back(Role::Creator);
            else if (*role == "Auditor") value.roles.push_back(Role::Auditor);
            else if (*role == "Member") value.roles.push_back(Role::Member);
            else return false;
        }
        book.members.push_back(std::move(value));
    }

    const auto* budget = Field(*object, "budget");
    if (!budget) return false;
    if (std::holds_alternative<std::nullptr_t>(budget->value)) return true;
    const auto* money = AsObject(*budget);
    Money value;
    if (!money || !ReadInt(*money, "cents", value.cents)) return false;
    book.budget = value;
    return true;
}

bool ReadEntry(const Json& json, Entry& entry) {
    const auto* object = AsObject(json);
    std::string state;
    std::string type;
    if (!object || !ReadInt(*object, "id", entry.id) ||
        !ReadInt(*object, "book_id", entry.book_id) ||
        !ReadInt(*object, "submitter_id", entry.submitter_id) ||
        !ReadString(*object, "state", state) ||
        !ReadString(*object, "type", type) ||
        !ReadInt(*object, "amount_cents", entry.amount.cents) ||
        !ReadTime(*object, "occurred_at", entry.occurred_at) ||
        !ReadWide(*object, "payer", entry.payer) ||
        !ReadWide(*object, "note", entry.note) ||
        !ReadTime(*object, "created_at", entry.created_at) ||
        !ReadOptionalTime(*object, "submitted_at", entry.submitted_at) ||
        !ReadOptionalTime(*object, "reviewed_at", entry.reviewed_at)) {
        return false;
    }
    if (state == "Draft") entry.state = EntryState::Draft;
    else if (state == "Waiting_review") entry.state = EntryState::Waiting_review;
    else if (state == "Rejected") entry.state = EntryState::Rejected;
    else if (state == "Accepted") entry.state = EntryState::Accepted;
    else if (state == "Deleted") entry.state = EntryState::Deleted;
    else return false;
    if (type == "Income") entry.type = EntryType::Income;
    else if (type == "expenditure") entry.type = EntryType::expenditure;
    else return false;

    const auto* attachments = ReadArray(*object, "attachments");
    if (!attachments) return false;
    for (const auto& item : *attachments) {
        const auto* path = std::get_if<std::string>(&item.value);
        std::wstring wide;
        if (!path || !FromUtf8(*path, wide)) return false;
        entry.attachments.emplace_back(wide);
    }

    const auto read_optional_id = [object](const char* key, auto& target) {
        const auto* json = Field(*object, key);
        if (!json) return false;
        if (std::holds_alternative<std::nullptr_t>(json->value)) {
            target.reset();
            return true;
        }
        const auto* id = std::get_if<std::int64_t>(&json->value);
        if (!id) return false;
        target = *id;
        return true;
    };
    return read_optional_id("reviewer_id", entry.reviewer_id) &&
           read_optional_id("replaces_entry_id", entry.replaces_entry_id);
}

bool ReadLog(const Json& json, OperationLog& log) {
    const auto* object = AsObject(json);
    if (!object || !ReadInt(*object, "id", log.id) ||
        !ReadInt(*object, "book_id", log.book_id) ||
        !ReadInt(*object, "actor_id", log.actor_id) ||
        !ReadWide(*object, "action", log.action) ||
        !ReadTime(*object, "operated_at", log.operated_at) ||
        !ReadString(*object, "before_json", log.before_json) ||
        !ReadString(*object, "after_json", log.after_json) ||
        !ReadWide(*object, "device_info", log.device_info)) {
        return false;
    }
    const auto* entry = Field(*object, "entry_id");
    if (!entry) return false;
    if (std::holds_alternative<std::nullptr_t>(entry->value)) {
        log.entry_id.reset();
        return true;
    }
    const auto* id = std::get_if<std::int64_t>(&entry->value);
    if (!id) return false;
    log.entry_id = *id;
    return true;
}

} // namespace

Result<std::string> SerializeSnapshot(const DatabaseSnapshot& snapshot) {
    try {
        std::string out = "{\"schema_version\":" +
                          std::to_string(snapshot.schema_version) + ",\"users\":[";
        for (std::size_t index = 0; index < snapshot.users.size(); ++index) {
            if (index != 0) out += ",";
            WriteUser(snapshot.users[index], out);
        }
        out += "],\"books\":[";
        for (std::size_t index = 0; index < snapshot.books.size(); ++index) {
            if (index != 0) out += ",";
            WriteBook(snapshot.books[index], out);
        }
        out += "],\"entries\":[";
        for (std::size_t index = 0; index < snapshot.entries.size(); ++index) {
            if (index != 0) out += ",";
            WriteEntry(snapshot.entries[index], out);
        }
        out += "],\"logs\":[";
        for (std::size_t index = 0; index < snapshot.logs.size(); ++index) {
            if (index != 0) out += ",";
            WriteLog(snapshot.logs[index], out);
        }
        out += "]}";
        return Result<std::string>{Status{}, std::move(out)};
    } catch (const std::exception&) {
        return Result<std::string>{
            Status{ErrorCode::DataCorrupted, "无法序列化快照。"},
            std::nullopt};
    }
}

Result<DatabaseSnapshot> DeserializeSnapshot(std::string_view json) {
    try {
        Json root;
        if (!JsonParser{json}.Parse(root)) {
            return Result<DatabaseSnapshot>{
                Status{ErrorCode::DataCorrupted, "快照 JSON 格式无效。"},
                std::nullopt};
        }
        const auto* object = AsObject(root);
        std::int64_t version{};
        if (!object || !ReadInt(*object, "schema_version", version) || version != 1) {
            return Result<DatabaseSnapshot>{
                Status{ErrorCode::DataCorrupted, "不支持此快照结构版本。"},
                std::nullopt};
        }
        const auto* users = ReadArray(*object, "users");
        const auto* books = ReadArray(*object, "books");
        const auto* entries = ReadArray(*object, "entries");
        const auto* logs = ReadArray(*object, "logs");
        if (!users || !books || !entries || !logs) {
            return Result<DatabaseSnapshot>{
                Status{ErrorCode::DataCorrupted, "快照缺少必要的数据集合。"},
                std::nullopt};
        }

        DatabaseSnapshot snapshot;
        snapshot.schema_version = static_cast<std::uint64_t>(version);
        for (const auto& item : *users) {
            User value;
            if (!ReadUser(item, value)) throw std::runtime_error("Invalid user.");
            snapshot.users.push_back(std::move(value));
        }
        for (const auto& item : *books) {
            Book value;
            if (!ReadBook(item, value)) throw std::runtime_error("Invalid book.");
            snapshot.books.push_back(std::move(value));
        }
        for (const auto& item : *entries) {
            Entry value;
            if (!ReadEntry(item, value)) throw std::runtime_error("Invalid entry.");
            snapshot.entries.push_back(std::move(value));
        }
        for (const auto& item : *logs) {
            OperationLog value;
            if (!ReadLog(item, value)) throw std::runtime_error("Invalid log.");
            snapshot.logs.push_back(std::move(value));
        }
        return Result<DatabaseSnapshot>{Status{}, std::move(snapshot)};
    } catch (const std::exception&) {
        return Result<DatabaseSnapshot>{
            Status{ErrorCode::DataCorrupted, "快照数据无效。"},
            std::nullopt};
    }
}

} // namespace ledger
