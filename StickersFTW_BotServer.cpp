/*
 * Part of StickersFTW BotServer, a simple HTTP server for serving Telegram
 * stickers.
 */

#define CPPHTTPLIB_OPENSSL_SUPPORT

#include <cxxopts.hpp>
#include <fmt/format.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <expected>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <ranges>
#include <thread>
#include <unordered_map>
#include <vector>

std::string make_api_url(const std::string_view token,
                         const std::string_view method) {
  return fmt::format("/bot{}/{}", token, method);
}

struct Sticker {
  std::string
      id; // Actually file_unique_id, but we will use it as id for our purposes
  std::string file_id; // hidden, not sent in JSON.
  enum class Format { Unknown, WebP, TGS, Jpeg, WebM } format;
  int width;
  int height;
  int size;
  std::optional<std::string> thumb;
  std::optional<std::string> thumb_file_id; // hidden, not sent in JSON.
  std::optional<std::string> emoji;

  struct Data {
    std::string data;
    Format format{};
    std::chrono::system_clock::time_point last_accessed =
        std::chrono::system_clock::now();

    bool isWebP() const {
      return data.size() >= 12 && data.substr(0, 4) == "RIFF" &&
             data.substr(8, 4) == "WEBP";
    }

    bool isWebM() const {
      return data.size() >= 4 && static_cast<unsigned char>(data[0]) == 0x1A &&
             static_cast<unsigned char>(data[1]) == 0x45 &&
             static_cast<unsigned char>(data[2]) == 0xDF &&
             static_cast<unsigned char>(data[3]) == 0xA3;
    }

    bool isTGS() const {
      return data.size() >= 2 && static_cast<unsigned char>(data[0]) == 0x1F &&
             static_cast<unsigned char>(data[1]) == 0x8B;
    }

    bool isJpeg() const {
      if (data.size() < 4) {
        return false;
      }

      const auto b0 = static_cast<unsigned char>(data[0]);
      const auto b1 = static_cast<unsigned char>(data[1]);
      const auto b2 = static_cast<unsigned char>(data[2]);
      const auto b3 = static_cast<unsigned char>(data[3]);

      // JPEG SOI marker followed by the beginning of another JPEG marker.
      return b0 == 0xFF && b1 == 0xD8 && b2 == 0xFF && b3 != 0x00 && b3 != 0xFF;
    }

    std::string mimeType() const {
      switch (format) {
      case Format::WebP:
        return "image/webp";
      case Format::WebM:
        return "video/webm";
      case Format::TGS:
        return "application/x-tgsticker";
      case Format::Jpeg:
        return "image/jpeg";
      default:
        return "application/octet-stream";
      }
    }

    Data(std::string data) : data(std::move(data)) {
      if (isWebP()) {
        format = Format::WebP;
      } else if (isWebM()) {
        format = Format::WebM;
      } else if (isTGS()) {
        format = Format::TGS;
      } else if (isJpeg()) {
        format = Format::Jpeg;
      } else {
        format = Format::Unknown;
      }
    }

    Data(std::string data, Format format) : Data(std::move(data)) {
      if (this->format != Format::Unknown && this->format != format) {
        spdlog::warn("Data format mismatch: detected {}, but provided {}",
                     static_cast<int>(this->format), static_cast<int>(format));
      }
    }

    Data() : data(""), format(Format::Unknown) {}
  };
};

struct GetStickerSetResponse {
  std::string name;
  std::string title;

  std::vector<Sticker> stickers;

  std::chrono::system_clock::time_point last_accessed =
      std::chrono::system_clock::now();

  static std::optional<GetStickerSetResponse>
  parse(const nlohmann::json &sticker_set_json) {
    GetStickerSetResponse sticker_set;
    sticker_set.name = sticker_set_json.value("name", "");
    sticker_set.title = sticker_set_json.value("title", "");

    spdlog::debug("Sticker set name: {}, title: '{}'", sticker_set.name,
                  sticker_set.title);
    if (sticker_set.name.empty() || sticker_set.title.empty()) {
      spdlog::error(
          "Invalid sticker set information received from Telegram API.");
      return std::nullopt;
    }

    for (const auto &sticker_json : sticker_set_json["stickers"]) {
      Sticker sticker;
      sticker.id = sticker_json.value("file_unique_id", "");
      sticker.file_id = sticker_json.value("file_id", "");
      sticker.width = sticker_json.value("width", 0);
      sticker.height = sticker_json.value("height", 0);
      sticker.size = sticker_json.value("file_size", 0);

      if (sticker.id.empty() || sticker.width <= 0 || sticker.height <= 0) {
        spdlog::error(
            "Invalid sticker information received from Telegram API.");
        return std::nullopt;
      }

      bool is_vid = sticker_json.value("is_video", false);
      bool is_anim = sticker_json.value("is_animated", false);

      sticker.format =
          is_vid ? Sticker::Format::WebM
                 : (is_anim ? Sticker::Format::TGS : Sticker::Format::WebP);

      if (sticker_json.contains("thumbnail")) {
        sticker.thumb = sticker_json["thumbnail"].value("file_unique_id", "");
        sticker.thumb_file_id = sticker_json["thumbnail"].value("file_id", "");
      }
      if (sticker_json.contains("emoji")) {
        sticker.emoji = sticker_json.value("emoji", "😡");
      }
      sticker_set.stickers.push_back(sticker);
    }
    return sticker_set;
  }
};

struct ErrorResponse {
  int error_code;
  std::string description;
  struct Parameters {
    std::optional<int> retry_after;
  };
  std::optional<Parameters> parameters;

  ErrorResponse(int code, std::string desc)
      : error_code(code), description(std::move(desc)) {}
  ErrorResponse() : error_code(0), description("") {}
};

template <typename BasicJsonType,
          nlohmann::detail::enable_if_t<
              nlohmann::detail::is_basic_json<BasicJsonType>::value, int> = 0>
void to_json(BasicJsonType &nlohmann_json_j, const Sticker &nlohmann_json_t) {
  nlohmann_json_j["id"] = nlohmann_json_t.id;
  nlohmann_json_j["width"] = nlohmann_json_t.width;
  nlohmann_json_j["height"] = nlohmann_json_t.height;
  nlohmann_json_j["size"] = nlohmann_json_t.size;
  if (nlohmann_json_t.thumb)
    nlohmann_json_j["thumb"] = nlohmann_json_t.thumb;
  if (nlohmann_json_t.emoji)
    nlohmann_json_j["emoji"] = nlohmann_json_t.emoji;
}
template <typename BasicJsonType,
          nlohmann::detail::enable_if_t<
              nlohmann::detail::is_basic_json<BasicJsonType>::value, int> = 0>
void from_json(const BasicJsonType &nlohmann_json_j, Sticker &nlohmann_json_t) {
  nlohmann_json_j.at("id").get_to(nlohmann_json_t.id);
  nlohmann_json_j.at("width").get_to(nlohmann_json_t.width);
  nlohmann_json_j.at("height").get_to(nlohmann_json_t.height);
  nlohmann_json_j.at("size").get_to(nlohmann_json_t.size);
  if (nlohmann_json_j.contains("thumb")) {
    nlohmann_json_j.at("thumb").get_to(nlohmann_json_t.thumb);
  }
  if (nlohmann_json_j.contains("emoji")) {
    nlohmann_json_j.at("emoji").get_to(nlohmann_json_t.emoji);
  }
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GetStickerSetResponse, name, title,
                                   stickers);
template <typename BasicJsonType,
          nlohmann::detail::enable_if_t<
              nlohmann::detail::is_basic_json<BasicJsonType>::value, int> = 0>
void to_json(BasicJsonType &nlohmann_json_j,
             const ErrorResponse &nlohmann_json_t) {
  nlohmann_json_j["error_code"] = nlohmann_json_t.error_code;
  nlohmann_json_j["description"] = nlohmann_json_t.description;
  if (nlohmann_json_t.parameters.has_value()) {
    nlohmann_json_j["parameters"] = nlohmann_json_t.parameters.value();
  }
}
template <typename BasicJsonType,
          nlohmann::detail::enable_if_t<
              nlohmann::detail::is_basic_json<BasicJsonType>::value, int> = 0>
void from_json(const BasicJsonType &nlohmann_json_j,
               ErrorResponse &nlohmann_json_t) {
  nlohmann_json_j.at("error_code").get_to(nlohmann_json_t.error_code);
  nlohmann_json_j.at("description").get_to(nlohmann_json_t.description);
  if (nlohmann_json_j.contains("parameters")) {
    nlohmann_json_j.at("parameters")
        .get_to(nlohmann_json_t.parameters.emplace());
  }
};
template <typename BasicJsonType,
          nlohmann::detail::enable_if_t<
              nlohmann::detail::is_basic_json<BasicJsonType>::value, int> = 0>
void to_json(BasicJsonType &nlohmann_json_j,
             const ErrorResponse::Parameters &nlohmann_json_t) {
  if (nlohmann_json_t.retry_after)
    nlohmann_json_j["retry_after"] = nlohmann_json_t.retry_after;
}
template <typename BasicJsonType,
          nlohmann::detail::enable_if_t<
              nlohmann::detail::is_basic_json<BasicJsonType>::value, int> = 0>
void from_json(const BasicJsonType &nlohmann_json_j,
               ErrorResponse::Parameters &nlohmann_json_t) {
  if (nlohmann_json_j.contains("retry_after")) {
    nlohmann_json_j.at("retry_after").get_to(nlohmann_json_t.retry_after);
  }
};

constexpr int HTTP_OK = 200;
constexpr int HTTP_CREATED = 201;
constexpr int HTTP_BAD_REQUEST = 400;
constexpr int HTTP_UNAUTHORIZED = 401;
constexpr int HTTP_FORBIDDEN = 403;
constexpr int HTTP_NOT_FOUND = 404;
constexpr int HTTP_TOO_MANY_REQUESTS = 429;
constexpr int HTTP_INTERNAL_SERVER_ERROR = 500;

// Writes an ErrorResponse as the actual JSON response body, so clients can
// see the real failure reason instead of a bare status code.
void writeError(httplib::Response &res, const ErrorResponse &err) {
  res.status = err.error_code;
  res.set_content(nlohmann::json(err).dump(), "application/json");
}

// Splits a comma-separated list (e.g. an emoji list) into trimmed,
// non-empty parts. Deliberately simple/manual rather than std::views::split
// to avoid any ambiguity in this codebase's C++23 ranges usage.
std::vector<std::string> splitCommaList(const std::string &raw) {
  std::vector<std::string> result;
  std::string current;
  for (char c : raw) {
    if (c == ',') {
      if (!current.empty()) {
        result.push_back(current);
        current.clear();
      }
    } else {
      current += c;
    }
  }
  if (!current.empty()) {
    result.push_back(current);
  }
  return result;
}

// Validates a client-supplied sticker set short name against Telegram's own
// naming rule (must start with a letter, letters/digits/underscores only)
// before it gets combined into "<name>_by_<bot_username>".
std::optional<std::string> sanitizeShortName(const std::string &raw) {
  if (raw.empty() || !std::isalpha(static_cast<unsigned char>(raw[0]))) {
    return std::nullopt;
  }
  for (char c : raw) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
      return std::nullopt;
    }
  }
  return raw;
}

class StickerFTWEngine {
  httplib::Client cli; // Client of Telegram API Server
  httplib::Server svr; // HTTP Server for serving requests

  std::mutex ratelimit_mutex; // Mutex for rate limit handling
  std::optional<std::chrono::system_clock::time_point>
      ratelimited_until; // Time point until which the Telegram API is rate
                         // limited

  std::mutex cache_mutex; // Mutex for sticker sets cache
  std::unordered_map<std::string /*name*/, GetStickerSetResponse>
      sticker_sets_cache; // Cache for sticker sets information

  std::mutex file_cache_mutex; // Mutex for sticker files cache
  std::unordered_map<std::string /*id*/, Sticker::Data>
      sticker_files_cache; // Cache for sticker files binary data

  // Token and API server URL for Telegram API
  std::string token;

  // Cached from the startup getMe() call; needed to build the canonical
  // "<name>_by_<bot_username>" sticker set name Telegram requires on create.
  std::string bot_username;

  using http_code_t = int;

public:
  explicit StickerFTWEngine(std::string token, std::string api_server)
      : cli(api_server.c_str()), svr(), token(token) {
    // Initialize the HTTP client and server
  }

  void shutdown() {
    svr.stop();
    cli.stop();
  }

  bool
  purgeCache(std::optional<std::chrono::system_clock::time_point> before_time) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    std::lock_guard<std::mutex> file_lock(file_cache_mutex);
    if (before_time.has_value()) {
      bool any_purged = false;
      for (auto it = sticker_sets_cache.begin();
           it != sticker_sets_cache.end();) {
        if (it->second.last_accessed < before_time.value()) {
          spdlog::debug("Purging sticker set '{}' from cache.", it->first);
          it = sticker_sets_cache.erase(it);
          any_purged = true;
        } else {
          ++it;
        }
      }
      for (auto it = sticker_files_cache.begin();
           it != sticker_files_cache.end();) {
        if (it->second.last_accessed < before_time.value()) {
          spdlog::debug("Purging sticker file '{}' from cache.", it->first);
          it = sticker_files_cache.erase(it);
          any_purged = true;
        } else {
          ++it;
        }
      }
      return any_purged;
    } else {
      size_t count = sticker_sets_cache.size() + sticker_files_cache.size();
      sticker_sets_cache.clear();
      sticker_files_cache.clear();
      spdlog::debug(
          "Purged all sticker sets and files from cache. Total purged: {}",
          count);
      return count > 0;
    }
  }

  static std::optional<std::expected<nlohmann::json, ErrorResponse>>
  unwrapTelegramBody(const std::string body) {
    nlohmann::json response_json;
    try {
      response_json = nlohmann::json::parse(body);
    } catch (const std::exception &e) {
      spdlog::error(
          "Failed to parse Telegram API response. Error: {}. Response: {}",
          e.what(), body);
      return std::nullopt;
    }
    if (!response_json.contains("ok")) {
      spdlog::error(
          "Telegram API response does not contain 'ok' field. Response: {}",
          body);
      return std::nullopt;
    }
    bool ok = response_json.value("ok", false);
    if (!ok) {
      spdlog::warn("Telegram API response not ok. Body: {}", body);
      try {
        ErrorResponse error_response =
            nlohmann::json::parse(body).get<ErrorResponse>();
        return std::unexpected(error_response);
      } catch (const std::exception &e) {
        spdlog::error("Failed to parse error response from Telegram API. "
                      "Error: {}. Response: {}",
                      e.what(), body);
        return std::unexpected(
            ErrorResponse(HTTP_INTERNAL_SERVER_ERROR,
                          "Failed to parse error response from Telegram API"));
      }
    }
    return response_json["result"];
  }

  std::expected<GetStickerSetResponse, ErrorResponse>
  getStickerSet(const std::string &sticker_set_name) {
    if (sticker_set_name.empty()) {
      spdlog::error("Sticker set name is empty.");
      return std::unexpected(
          ErrorResponse(HTTP_BAD_REQUEST, "Sticker set name is empty."));
    }

    spdlog::debug("Received request for sticker set: {}", sticker_set_name);

    {
      std::lock_guard<std::mutex> lock(cache_mutex);
      auto it = sticker_sets_cache.find(sticker_set_name);
      if (it != sticker_sets_cache.end()) {
        spdlog::debug("Sticker set '{}' found in cache.", sticker_set_name);
        // Hit the cache timestamp
        it->second.last_accessed = std::chrono::system_clock::now();
        // Return the cached sticker set information as JSON
        return it->second;
      }
    }

    // Check if the Telegram API server is currently rate limited
    if (is_ratelimited()) {
      spdlog::warn(
          "Rate limit in effect. Rejecting request for sticker set: {}",
          sticker_set_name);
      return std::unexpected(
          ErrorResponse(HTTP_TOO_MANY_REQUESTS, "Rate limit in effect."));
    }

    // Call Telegram API to get sticker set information
    auto response =
        cli.Get(make_api_url(token, "getStickerSet?name=" + sticker_set_name));

    /*
     * Check if the response is valid.
     *
     * Telegram specifies:
     * 400 Bad Request: STICKERSET_INVALID: No sticker set exists with the given
     * name, or the name is spelled wrong or has the wrong case. 400 Bad
     * Request: invalid sticker set name: The provided name contains illegal
     *                  characters (must be alphanumeric and underscores, ending
     * with _by_<botname>). 401 Unauthorized: The bot token provided in the
     * request header is invalid or has been revoked. 429 Too Many Requests:
     * Rate limit or flood control hit; check the retry_after parameter in the
     *                        response body to see how long to wait before
     * trying again
     */

    if (!response) {
      spdlog::error("Failed to get response from Telegram API. Error code: {}",
                    static_cast<int>(response.error()));
      return std::unexpected(
          ErrorResponse(HTTP_INTERNAL_SERVER_ERROR,
                        "Failed to get response from Telegram API"));
    }
    spdlog::debug("Telegram API response status: {}", response->status);
    auto body_opt = unwrapTelegramBody(response->body);
    if (!body_opt) {
      spdlog::error(
          "Failed to unwrap Telegram API response body for sticker set '{}'.",
          sticker_set_name);
      return std::unexpected(
          ErrorResponse(HTTP_INTERNAL_SERVER_ERROR,
                        "Failed to unwrap Telegram API response body"));
    }
    auto body = body_opt.value();

    if (response->status == HTTP_OK) {
      spdlog::debug(
          "Successfully retrieved sticker set '{}' from Telegram API.",
          sticker_set_name);
      spdlog::debug("Telegram API response body: {}", body->dump());

      // Not found in cache, parse the response from Telegram API
      auto sticker_set_opt = GetStickerSetResponse::parse(*body);
      if (!sticker_set_opt) {
        return std::unexpected(ErrorResponse(
            HTTP_INTERNAL_SERVER_ERROR,
            "Failed to parse sticker set information from Telegram API"));
      }

      spdlog::debug("Parsed sticker set information successfully.");

      GetStickerSetResponse sticker_set = sticker_set_opt.value();
      {
        std::lock_guard<std::mutex> lock(cache_mutex);
        // Cache the sticker set information for future requests
        sticker_sets_cache[sticker_set.name] = sticker_set;
      }
      return sticker_set;
    }

    // Handle error responses from Telegram API
    return std::unexpected(http_code_err_handle(
        body.error(), fmt::format("getStickerSet for '{}'", sticker_set_name)));
  }

  void append_ratelimit(int retry_after) {
    std::lock_guard<std::mutex> lock(ratelimit_mutex);
    if (ratelimited_until.has_value()) {
      ratelimited_until = std::max(ratelimited_until.value(),
                                   std::chrono::system_clock::now() +
                                       std::chrono::seconds(retry_after));
    } else {
      ratelimited_until =
          std::chrono::system_clock::now() + std::chrono::seconds(retry_after);
    }
  }

  bool is_ratelimited() {
    std::lock_guard<std::mutex> lock(ratelimit_mutex);
    return ratelimited_until.has_value() &&
           std::chrono::system_clock::now() < ratelimited_until.value();
  }

  ErrorResponse http_code_err_handle(const ErrorResponse &resp,
                                     const std::string_view desc) {
    switch (resp.error_code) {
    case HTTP_OK: {
      return ErrorResponse(resp.error_code, "Success"); // Should not reach
    }
    case HTTP_TOO_MANY_REQUESTS: {
      if (!resp.parameters || !resp.parameters->retry_after) {
        spdlog::warn("Telegram API server error for {}. Status code: {}. "
                     "Missing retry_after parameter.",
                     desc, resp.error_code);
      } else {
        append_ratelimit(*resp.parameters->retry_after);
      }
      [[fallthrough]];
    }
    default: {
      spdlog::error("Telegram API server error for {}. Status code: {}", desc,
                    resp.error_code);
      // Filter request fault and server fault codes, and return the appropriate
      // HTTP status code.
      switch (resp.error_code) {
      case HTTP_BAD_REQUEST:
      case HTTP_NOT_FOUND:
      case HTTP_TOO_MANY_REQUESTS:
        // Preserve Telegram's real description (e.g. "STICKERSET_INVALID",
        // "PEER_ID_INVALID") instead of a generic message -- callers (in
        // particular pushSticker) inspect this text, and clients benefit
        // from seeing the real reason now that errors carry a JSON body.
        return ErrorResponse(resp.error_code, resp.description);
      default:
        return ErrorResponse(HTTP_INTERNAL_SERVER_ERROR,
                             "Internal server error.");
      }
    }
    }
  }

  std::expected<Sticker::Data, ErrorResponse>
  getStickerFile(const std::string &file_id) {
    if (file_id.empty()) {
      spdlog::error("Sticker file_id is empty.");
      return std::unexpected(
          ErrorResponse(HTTP_BAD_REQUEST, "Sticker file_id is empty."));
    }
    spdlog::debug("Received request for sticker file with file_id: {}",
                  file_id);
    {
      std::lock_guard<std::mutex> lock(file_cache_mutex);
      auto it = sticker_files_cache.find(file_id);
      if (it != sticker_files_cache.end()) {
        spdlog::debug("Sticker file with file_id '{}' found in cache.",
                      file_id);
        it->second.last_accessed = std::chrono::system_clock::now();
        return it->second.data;
      }
    }

    // Check if the Telegram API server is currently rate limited
    if (is_ratelimited()) {
      spdlog::warn("Rate limit in effect. Rejecting request for sticker file "
                   "with file_id: {}",
                   file_id);
      return std::unexpected(
          ErrorResponse(HTTP_TOO_MANY_REQUESTS, "Rate limit in effect."));
    }

    auto response = cli.Get(make_api_url(token, "getFile?file_id=" + file_id));
    if (!response) {
      spdlog::error("Failed to get response from Telegram API for sticker "
                    "file. Error code: {}",
                    static_cast<int>(response.error()));
      return std::unexpected(
          ErrorResponse(HTTP_INTERNAL_SERVER_ERROR,
                        "Failed to get response from Telegram API."));
    }
    auto body_opt = unwrapTelegramBody(response->body);
    if (!body_opt) {
      spdlog::error("Failed to unwrap Telegram API response body for sticker "
                    "file with file_id '{}'.",
                    file_id);
      return std::unexpected(
          ErrorResponse(HTTP_INTERNAL_SERVER_ERROR,
                        "Failed to unwrap Telegram API response body"));
    }
    auto body = body_opt.value();

    spdlog::debug("Telegram API response status for sticker file: {}",
                  response->status);
    if (response->status == HTTP_OK) {
      spdlog::debug("Successfully retrieved sticker file with file_id '{}' "
                    "from Telegram API.",
                    file_id);
      std::string file_path = (*body)["file_path"];
      if (file_path.empty()) {
        spdlog::error("Telegram API response for sticker file with file_id "
                      "'{}' does not contain a valid file_path.",
                      file_id);
        return std::unexpected(ErrorResponse(
            HTTP_INTERNAL_SERVER_ERROR,
            "Failed to retrieve sticker file path from Telegram API."));
      }
      // Now download the actual file using the file_path
      auto file_response =
          cli.Get(fmt::format("/file/bot{}/{}", token, file_path));
      if (!file_response) {
        spdlog::error("Failed to download sticker file with file_id '{}' from "
                      "Telegram API. Error code: {}",
                      file_id, static_cast<int>(file_response.error()));
        return std::unexpected(ErrorResponse(
            HTTP_INTERNAL_SERVER_ERROR,
            "Failed to download sticker file from Telegram API."));
      }
      if (file_response->status != HTTP_OK) {
        return std::unexpected(http_code_err_handle(
            ErrorResponse(file_response->status,
                          "Failed to download sticker file from Telegram API."),
            fmt::format("download sticker file with file_id '{}'", file_id)));
      }
      spdlog::debug("Successfully downloaded sticker file with file_id '{}' "
                    "from Telegram API.",
                    file_id);
      // Cache the sticker file binary data for future requests
      {
        std::lock_guard<std::mutex> lock(file_cache_mutex);
        sticker_files_cache[file_id] = Sticker::Data(file_response->body);
      }
      return Sticker::Data(file_response->body);
    }
    return std::unexpected(http_code_err_handle(
        body.error(),
        fmt::format("getFile for sticker file with file_id '{}'", file_id)));
  }

  // Uploads raw sticker bytes to Telegram, returning the resulting file_id
  // that createNewStickerSet/addStickerToSet can then reference without
  // re-uploading the binary.
  std::expected<std::string, ErrorResponse>
  uploadStickerFile(const std::string &user_id, const std::string &sticker_data,
                    const std::string &sticker_format,
                    const std::string &filename) {
    if (is_ratelimited()) {
      return std::unexpected(
          ErrorResponse(HTTP_TOO_MANY_REQUESTS, "Rate limit in effect."));
    }

    httplib::UploadFormDataItems items = {
        {"user_id", user_id, "", ""},
        {"sticker_format", sticker_format, "", ""},
        {"sticker", sticker_data, filename, "application/octet-stream"},
    };
    auto response = cli.Post(make_api_url(token, "uploadStickerFile"), items);
    if (!response) {
      spdlog::error("Failed to reach Telegram API for uploadStickerFile. "
                    "Error code: {}",
                    static_cast<int>(response.error()));
      return std::unexpected(ErrorResponse(
          HTTP_INTERNAL_SERVER_ERROR,
          "Failed to reach Telegram API for uploadStickerFile."));
    }
    auto body_opt = unwrapTelegramBody(response->body);
    if (!body_opt) {
      return std::unexpected(
          ErrorResponse(HTTP_INTERNAL_SERVER_ERROR,
                        "Failed to unwrap uploadStickerFile response body."));
    }
    auto body = body_opt.value();
    if (response->status != HTTP_OK) {
      return std::unexpected(
          http_code_err_handle(body.error(), "uploadStickerFile"));
    }
    std::string file_id = body->value("file_id", "");
    if (file_id.empty()) {
      return std::unexpected(
          ErrorResponse(HTTP_INTERNAL_SERVER_ERROR,
                        "uploadStickerFile response did not contain a file_id."));
    }
    return file_id;
  }

  // Creates a brand-new sticker set owned by user_id, seeded with a single
  // already-uploaded sticker (referenced by file_id).
  std::expected<void, ErrorResponse>
  createNewStickerSet(const std::string &user_id, const std::string &full_name,
                      const std::string &title, const std::string &file_id,
                      const std::string &sticker_format,
                      const std::vector<std::string> &emojis) {
    if (is_ratelimited()) {
      return std::unexpected(
          ErrorResponse(HTTP_TOO_MANY_REQUESTS, "Rate limit in effect."));
    }

    nlohmann::json input_sticker = {
        {"sticker", file_id},
        {"format", sticker_format},
        {"emoji_list", emojis},
    };
    nlohmann::json stickers_arr = nlohmann::json::array({input_sticker});
    httplib::Params params{
        {"user_id", user_id},
        {"name", full_name},
        {"title", title},
        {"stickers", stickers_arr.dump()},
    };
    auto response = cli.Post(make_api_url(token, "createNewStickerSet"), params);
    if (!response) {
      return std::unexpected(ErrorResponse(
          HTTP_INTERNAL_SERVER_ERROR,
          "Failed to reach Telegram API for createNewStickerSet."));
    }
    auto body_opt = unwrapTelegramBody(response->body);
    if (!body_opt) {
      return std::unexpected(
          ErrorResponse(HTTP_INTERNAL_SERVER_ERROR,
                        "Failed to unwrap createNewStickerSet response body."));
    }
    auto body = body_opt.value();
    if (response->status != HTTP_OK) {
      return std::unexpected(
          http_code_err_handle(body.error(), "createNewStickerSet"));
    }
    return {};
  }

  // Appends a single already-uploaded sticker (by file_id) to an existing
  // set owned by user_id.
  std::expected<void, ErrorResponse>
  addStickerToSet(const std::string &user_id, const std::string &full_name,
                  const std::string &file_id, const std::string &sticker_format,
                  const std::vector<std::string> &emojis) {
    if (is_ratelimited()) {
      return std::unexpected(
          ErrorResponse(HTTP_TOO_MANY_REQUESTS, "Rate limit in effect."));
    }

    nlohmann::json input_sticker = {
        {"sticker", file_id},
        {"format", sticker_format},
        {"emoji_list", emojis},
    };
    httplib::Params params{
        {"user_id", user_id},
        {"name", full_name},
        {"sticker", input_sticker.dump()},
    };
    auto response = cli.Post(make_api_url(token, "addStickerToSet"), params);
    if (!response) {
      return std::unexpected(
          ErrorResponse(HTTP_INTERNAL_SERVER_ERROR,
                        "Failed to reach Telegram API for addStickerToSet."));
    }
    auto body_opt = unwrapTelegramBody(response->body);
    if (!body_opt) {
      return std::unexpected(
          ErrorResponse(HTTP_INTERNAL_SERVER_ERROR,
                        "Failed to unwrap addStickerToSet response body."));
    }
    auto body = body_opt.value();
    if (response->status != HTTP_OK) {
      return std::unexpected(
          http_code_err_handle(body.error(), "addStickerToSet"));
    }
    return {};
  }

  struct PushResult {
    GetStickerSetResponse set;
    bool created;
  };

  // Orchestrates a full "push a sticker to Telegram" request: uploads the
  // file, tries to append it to an existing set, and falls back to creating
  // a brand-new set (only if the caller supplied a title) when the set
  // doesn't exist yet. This is what backs POST /v1/set/{name}/.
  std::expected<PushResult, ErrorResponse>
  pushSticker(const std::string &short_name, const std::string &user_id,
             const std::optional<std::string> &title,
             const std::string &sticker_data, const std::string &sticker_format,
             const std::vector<std::string> &emojis) {
    if (user_id.empty()) {
      return std::unexpected(
          ErrorResponse(HTTP_BAD_REQUEST, "user_id is required."));
    }
    if (sticker_format != "static" && sticker_format != "video") {
      return std::unexpected(
          ErrorResponse(HTTP_BAD_REQUEST, "format must be 'static' or 'video'."));
    }
    if (emojis.empty() || emojis.size() > 3) {
      return std::unexpected(
          ErrorResponse(HTTP_BAD_REQUEST, "1 to 3 emojis are required."));
    }
    auto sanitized = sanitizeShortName(short_name);
    if (!sanitized) {
      return std::unexpected(ErrorResponse(
          HTTP_BAD_REQUEST,
          "Invalid sticker set name. Must start with a letter and contain "
          "only letters, digits and underscores."));
    }
    if (bot_username.empty()) {
      return std::unexpected(
          ErrorResponse(HTTP_INTERNAL_SERVER_ERROR, "Bot username unavailable."));
    }
    std::string full_name = *sanitized + "_by_" + bot_username;
    if (full_name.size() > 64) {
      return std::unexpected(
          ErrorResponse(HTTP_BAD_REQUEST, "Sticker set name too long."));
    }

    std::string filename = "sticker." + std::string(
        sticker_format == "video" ? "webm" : "webp");
    auto file_id_result =
        uploadStickerFile(user_id, sticker_data, sticker_format, filename);
    if (!file_id_result) {
      return std::unexpected(file_id_result.error());
    }
    std::string file_id = file_id_result.value();

    bool created = false;
    auto add_result =
        addStickerToSet(user_id, full_name, file_id, sticker_format, emojis);
    if (!add_result) {
      bool set_missing =
          add_result.error().description.find("STICKERSET_INVALID") !=
          std::string::npos;
      if (set_missing && title.has_value() && !title->empty()) {
        auto create_result = createNewStickerSet(
            user_id, full_name, *title, file_id, sticker_format, emojis);
        if (!create_result) {
          return std::unexpected(create_result.error());
        }
        created = true;
      } else if (set_missing) {
        return std::unexpected(ErrorResponse(
            HTTP_BAD_REQUEST,
            "Sticker set does not exist yet; provide 'title' to create it."));
      } else {
        return std::unexpected(add_result.error());
      }
    }

    // The set changed on Telegram's side -- drop any cached copy so the
    // re-fetch below reflects the push we just made.
    {
      std::lock_guard<std::mutex> lock(cache_mutex);
      sticker_sets_cache.erase(full_name);
    }
    auto refreshed = getStickerSet(full_name);
    if (!refreshed) {
      return std::unexpected(refreshed.error());
    }
    return PushResult{refreshed.value(), created};
  }

  bool listen(std::string ipaddr, const int port) {
    // Try a quick #getMe, testing both the token and the API server URL, before
    // starting the server.
    auto getMeRes =
        cli.Post(make_api_url(token, "getMe"), "", "application/json");
    if (!getMeRes || getMeRes->status != HTTP_OK) {
      spdlog::error("Failed to get response from Telegram API for getMe");
      if (getMeRes) {
        spdlog::error("Telegram API response status: {}", getMeRes->status);
      }
      return false;
    }

    // Capture the bot's own username, needed to build canonical
    // "<name>_by_<bot_username>" sticker set names for push support.
    auto getMeBodyOpt = unwrapTelegramBody(getMeRes->body);
    if (!getMeBodyOpt || !getMeBodyOpt->has_value()) {
      spdlog::error("Failed to parse getMe response body.");
      return false;
    }
    bot_username = getMeBodyOpt->value().value("username", "");
    if (bot_username.empty()) {
      spdlog::error("getMe response did not include a bot username.");
      return false;
    }
    spdlog::info("Authenticated as bot @{}", bot_username);

    // Thumbnail image get
    svr.Get("/v1/set/([^/]+)/([^/]+)/thumbnail/?",
            [&](const httplib::Request &req, httplib::Response &res) {
      std::string sticker_set_name = req.matches[1];
      std::string sticker_id = req.matches[2];

      spdlog::debug(
          "Received thumbnail request for sticker set: '{}', sticker id: '{}'",
          sticker_set_name, sticker_id);

      auto sticker_set_result = getStickerSet(sticker_set_name);
      if (!sticker_set_result) {
        // Set not found? Return the error from getStickerSet
        writeError(res, sticker_set_result.error());
        return;
      } else {
        auto sticker_set = sticker_set_result.value();
        auto sticker_file_it = std::ranges::find_if(
            sticker_set.stickers,
            [&](const Sticker &sticker) { return sticker.id == sticker_id; });
        if (sticker_file_it == sticker_set.stickers.end()) {
          spdlog::warn("Sticker with id '{}' not found in sticker set '{}'.",
                       sticker_id, sticker_set_name);
          writeError(res, ErrorResponse(HTTP_NOT_FOUND,
                                        "Sticker not found in this set."));
          return;
        }
        if (!sticker_file_it->thumb_file_id) {
          spdlog::warn("This sticker does not have thumbnail");
          writeError(res, ErrorResponse(HTTP_NOT_FOUND,
                                        "This sticker has no thumbnail."));
          return;
        }
        auto sticker_file_result =
            getStickerFile(*sticker_file_it->thumb_file_id);
        if (!sticker_file_result) {
          writeError(res, sticker_file_result.error());
          return;
        } else {
          auto sticker_data = sticker_file_result.value();
          res.set_content(sticker_data.data, sticker_data.mimeType());
        }
      }
    });

    // Sticker image get
    svr.Get("/v1/set/([^/]+)/([^/]+)/?",
            [&](const httplib::Request &req, httplib::Response &res) {
      std::string sticker_set_name = req.matches[1];
      std::string sticker_id = req.matches[2];

      spdlog::debug("Received request for sticker set: '{}', sticker id: '{}'",
                    sticker_set_name, sticker_id);

      auto sticker_set_result = getStickerSet(sticker_set_name);
      if (!sticker_set_result) {
        // Set not found? Return the error from getStickerSet
        writeError(res, sticker_set_result.error());
        return;
      } else {
        auto sticker_set = sticker_set_result.value();
        auto sticker_file_it = std::ranges::find_if(
            sticker_set.stickers,
            [&](const Sticker &sticker) { return sticker.id == sticker_id; });
        if (sticker_file_it == sticker_set.stickers.end()) {
          spdlog::warn("Sticker with id '{}' not found in sticker set '{}'.",
                       sticker_id, sticker_set_name);
          writeError(res, ErrorResponse(HTTP_NOT_FOUND,
                                        "Sticker not found in this set."));
          return;
        }
        auto sticker_file_result = getStickerFile(sticker_file_it->file_id);
        if (!sticker_file_result) {
          writeError(res, sticker_file_result.error());
          return;
        } else {
          auto sticker_data = sticker_file_result.value();
          res.set_content(sticker_data.data, sticker_data.mimeType());
        }
      }
    });

    // Sticker set get
    svr.Get("/v1/set/([^/]+)/?",
            [&](const httplib::Request &req, httplib::Response &res) {
          std::string sticker_set_name = req.matches[1];

          spdlog::debug("Received request for sticker set: '{}'", sticker_set_name);

          auto sticker_set_result = getStickerSet(sticker_set_name);
          if (!sticker_set_result) {
            writeError(res, sticker_set_result.error());
          } else {
            res.set_content(nlohmann::json(sticker_set_result.value()).dump(),
                            "application/json");
          }
        });

    // Bot identity get (purely informational -- lets clients show the
    // correct "@bot_username" in their own UI instructions).
    svr.Get("/v1/bot/?", [&](const httplib::Request &req,
                             httplib::Response &res) {
      (void)req;
      nlohmann::json j;
      j["username"] = bot_username;
      res.set_content(j.dump(), "application/json");
    });

    // Push a sticker to Telegram: appends to an existing set the caller
    // owns, or creates a brand-new one (if "title" is supplied and the set
    // doesn't exist yet).
    svr.Post("/v1/set/([A-Za-z][A-Za-z0-9_]*)/?",
             [&](const httplib::Request &req, httplib::Response &res) {
      std::string short_name = req.matches[1];

      if (!req.form.has_field("user_id") || !req.form.has_field("format") ||
          !req.form.has_field("emojis") || !req.form.has_file("sticker")) {
        writeError(res, ErrorResponse(
                            HTTP_BAD_REQUEST,
                            "Missing required fields: user_id, format, "
                            "emojis, sticker (file)."));
        return;
      }

      std::string user_id = req.form.get_field("user_id");
      std::string format = req.form.get_field("format");
      std::string emojis_raw = req.form.get_field("emojis");
      std::optional<std::string> title;
      if (req.form.has_field("title")) {
        std::string title_value = req.form.get_field("title");
        if (!title_value.empty()) {
          title = title_value;
        }
      }
      auto emojis = splitCommaList(emojis_raw);
      auto sticker_file = req.form.get_file("sticker");

      spdlog::debug("Received push request for sticker set short name: '{}'",
                    short_name);

      auto result = pushSticker(short_name, user_id, title,
                                sticker_file.content, format, emojis);
      if (!result) {
        writeError(res, result.error());
        return;
      }

      res.status = result.value().created ? HTTP_CREATED : HTTP_OK;
      res.set_content(nlohmann::json(result.value().set).dump(),
                      "application/json");
    });

    return svr.listen(ipaddr, port, 0);
  }
};

std::atomic<bool> running(true);

#ifdef WIN32
#include <windows.h>
int WINAPI console_ctrl_handler(DWORD ctrl_type) {
  switch (ctrl_type) {
  case CTRL_C_EVENT:
  case CTRL_CLOSE_EVENT:
  case CTRL_BREAK_EVENT:
  case CTRL_LOGOFF_EVENT:
  case CTRL_SHUTDOWN_EVENT:
    running.store(false);
    return TRUE;
  default:
    return FALSE;
  }
}
#elif defined(__unix__) || defined(__APPLE__) || defined(__linux__)
#include <csignal>
void signal_handler(int signal) { running.store(false); }
#endif

int main(int argc, char *argv[]) {
  cxxopts::Options options(
      "StickersFTW BotServer",
      "A simple HTTP server for serving Telegram stickers.");
  options.add_options()("p,port", "Port to run the server on",
                        cxxopts::value<int>()->default_value("8080"))(
      "h,host", "Host to run the server on",
      cxxopts::value<std::string>()->default_value("localhost"))(
      "t,token", "Telegram bot token", cxxopts::value<std::string>())(
      "l,log-level", "Log level (debug, info, warning, error, critical)",
      cxxopts::value<std::string>()->default_value("info"))(
      "s,server", "Telegram API server URL",
      cxxopts::value<std::string>())("help", "Print help");

  cxxopts::ParseResult result;

  try {
    result = options.parse(argc, argv);
  } catch (const cxxopts::exceptions::exception &e) {
    spdlog::error("Error parsing options: {}", e.what());
    return EXIT_FAILURE;
  }

  if (result.count("help")) {
    spdlog::info("{}", options.help());
    return EXIT_SUCCESS;
  }

  if (!result.count("token")) {
    spdlog::error("Telegram bot token is required.");
    return EXIT_FAILURE;
  }

  std::string token = result["token"].as<std::string>();
  std::string host = result["host"].as<std::string>();
  int port = result["port"].as<int>();
  std::string api_server = "https://api.telegram.org";

  if (result.count("server")) {
    api_server = result["server"].as<std::string>();
    spdlog::info("Using custom API server: {}", api_server);
  }

  auto log_level = result["log-level"].as<std::string>();
  if (log_level == "debug") {
    spdlog::set_level(spdlog::level::debug);
  } else if (log_level == "info") {
    spdlog::set_level(spdlog::level::info);
  } else if (log_level == "warning") {
    spdlog::set_level(spdlog::level::warn);
  } else if (log_level == "error") {
    spdlog::set_level(spdlog::level::err);
  } else if (log_level == "critical") {
    spdlog::set_level(spdlog::level::critical);
  } else {
    spdlog::error("Invalid log level.");
    return EXIT_FAILURE;
  }

#ifdef WIN32
  SetConsoleCtrlHandler(&console_ctrl_handler, TRUE);
#elif defined(__unix__) || defined(__APPLE__) || defined(__linux__)
  signal(SIGINT, signal_handler);
#endif

  spdlog::info("Starting StickersFTW BotServer on {}:{}", host, port);

  std::stop_source stop_source;
  auto stop_token_signal = stop_source.get_token();
  auto stop_token_cache = stop_source.get_token();
  std::mutex stop_mutex_signal;
  std::mutex stop_mutex_cache;
  std::condition_variable_any stop_cv_signal;
  std::condition_variable_any stop_cv_cache;

  StickerFTWEngine engine(token, api_server);
  std::jthread signal_catcher([&]() {
    while (running && !stop_token_signal.stop_requested()) {
      std::unique_lock<std::mutex> lock(stop_mutex_signal);
      stop_cv_signal.wait_for(
          lock, stop_token_signal, std::chrono::seconds(1),
          [stop_token_signal] { return stop_token_signal.stop_requested(); });
    }
    spdlog::info("Signal received, shutting down server...");
    engine.shutdown();
  });
  std::jthread cache_purger([&]() {
    while (running && !stop_token_cache.stop_requested()) {
      std::unique_lock<std::mutex> lock(stop_mutex_cache);
      if (stop_cv_cache.wait_for(lock, stop_token_cache, std::chrono::days(1),
                                 [stop_token_cache] {
                                   return stop_token_cache.stop_requested();
                                 })) {
        break;
      }
      engine.purgeCache(std::nullopt);
    }
  });

  if (!engine.listen(host, port)) {
    running = false;
    spdlog::error("Failed to start server on {}:{}", host, port);
  }
  stop_source.request_stop();

  spdlog::info("Shutting down server...");
  return EXIT_SUCCESS;
}