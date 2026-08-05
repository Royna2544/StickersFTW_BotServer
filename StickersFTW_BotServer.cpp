/*
 * Part of StickersFTW BotServer, a simple HTTP server for serving Telegram stickers.
 */

#define CPPHTTPLIB_OPENSSL_SUPPORT

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <httplib.h>
#include <cxxopts.hpp>
#include <fmt/format.h>

#include <atomic>
#include <iostream>
#include <queue>
#include <functional>
#include <thread>
#include <mutex>
#include <vector>
#include <chrono>
#include <expected>
#include <ranges>
#include <unordered_map>
#include <optional>
#include <cmath>

std::string make_api_url(const std::string_view token, const std::string_view method) {
	return fmt::format("/bot{}/{}", token, method);
}

struct Sticker {
	std::string id; // Actually file_unique_id, but we will use it as id for our purposes
	std::string file_id; // hidden, not sent in JSON.		
	enum class Format {
		Unknown,
		WebP,
		TGS,
		Jpeg,
		WebM
	} format;
	int width;
	int height;
	int size;
	std::optional<std::string> thumb;
	std::optional<std::string> thumb_file_id; // hidden, not sent in JSON.
	std::optional<std::string> emoji;

	struct Data {
		std::string data;
		Format format{};
		std::chrono::system_clock::time_point last_accessed = std::chrono::system_clock::now();

		bool isWebP() const {
			return data.size() >= 12 &&
				data.substr(0, 4) == "RIFF" &&
				data.substr(8, 4) == "WEBP";
		}

		bool isWebM() const {
			return data.size() >= 4 &&
				static_cast<unsigned char>(data[0]) == 0x1A &&
				static_cast<unsigned char>(data[1]) == 0x45 &&
				static_cast<unsigned char>(data[2]) == 0xDF &&
				static_cast<unsigned char>(data[3]) == 0xA3;
		}

		bool isTGS() const {
			return data.size() >= 2 &&
				static_cast<unsigned char>(data[0]) == 0x1F &&
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
			return b0 == 0xFF &&
				b1 == 0xD8 &&
				b2 == 0xFF &&
				b3 != 0x00 &&
				b3 != 0xFF;
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
			}
			else if (isWebM()) {
				format = Format::WebM;
			}
			else if (isTGS()) {
				format = Format::TGS;
			}
			else if (isJpeg()) {
				format = Format::Jpeg;
			}
			else {
				format = Format::Unknown;
			}
		}

		Data(std::string data, Format format) : Data(std::move(data)) {
			if (this->format != Format::Unknown && this->format != format) {
				spdlog::warn("Data format mismatch: detected {}, but provided {}", static_cast<int>(this->format), static_cast<int>(format));
			}
		}

		Data() : data(""), format(Format::Unknown) {}
	};
};

struct GetStickerSetResponse {
	std::string name;
	std::string title;
	
	std::vector<Sticker> stickers;

	std::chrono::system_clock::time_point last_accessed = std::chrono::system_clock::now();

	static std::optional<GetStickerSetResponse> parse(const nlohmann::json& sticker_set_json) {
		GetStickerSetResponse sticker_set;
		sticker_set.name = sticker_set_json.value("name", "");
		sticker_set.title = sticker_set_json.value("title", "");

		spdlog::debug("Sticker set name: {}, title: '{}'", sticker_set.name, sticker_set.title);
		if (sticker_set.name.empty() || sticker_set.title.empty()) {
			spdlog::error("Invalid sticker set information received from Telegram API.");
			return std::nullopt;
		}

		for (const auto& sticker_json : sticker_set_json["stickers"]) {
			Sticker sticker;
			sticker.id = sticker_json.value("file_unique_id", "");
			sticker.file_id = sticker_json.value("file_id", "");
			sticker.width = sticker_json.value("width", 0);
			sticker.height = sticker_json.value("height", 0);
			sticker.size = sticker_json.value("file_size", 0);

			if (sticker.id.empty() || sticker.width <= 0 || sticker.height <= 0) {
				spdlog::error("Invalid sticker information received from Telegram API.");
				return std::nullopt;
			}

			bool is_vid = sticker_json.value("is_video", false);
			bool is_anim = sticker_json.value("is_animated", false);

			sticker.format = is_vid ? Sticker::Format::WebM : (is_anim ? Sticker::Format::TGS : Sticker::Format::WebP);

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

	ErrorResponse(int code, std::string desc) : error_code(code), description(std::move(desc)) {}
	ErrorResponse() : error_code(0), description("") {}
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Sticker, id, width, height, size, thumb, emoji);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GetStickerSetResponse, name, title, stickers);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ErrorResponse, error_code, description, parameters);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ErrorResponse::Parameters, retry_after);

constexpr int HTTP_OK = 200;
constexpr int HTTP_BAD_REQUEST = 400;
constexpr int HTTP_UNAUTHORIZED = 401;
constexpr int HTTP_FORBIDDEN = 403;
constexpr int HTTP_NOT_FOUND = 404;
constexpr int HTTP_TOO_MANY_REQUESTS = 429;
constexpr int HTTP_INTERNAL_SERVER_ERROR = 500;

class StickerFTWEngine {
	httplib::Client cli; // Client of Telegram API Server
	httplib::Server svr; // HTTP Server for serving requests

	std::mutex ratelimit_mutex; // Mutex for rate limit handling
	std::optional<std::chrono::system_clock::time_point> ratelimited_until; // Time point until which the Telegram API is rate limited

	std::mutex cache_mutex; // Mutex for sticker sets cache
	std::unordered_map<std::string /*name*/, GetStickerSetResponse> sticker_sets_cache; // Cache for sticker sets information

	std::mutex file_cache_mutex; // Mutex for sticker files cache
	std::unordered_map<std::string /*id*/, Sticker::Data> sticker_files_cache; // Cache for sticker files binary data

	// Token and API server URL for Telegram API
	std::string token;

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

	bool purgeCache(std::optional<std::chrono::system_clock::time_point> before_time) {
		std::lock_guard<std::mutex> lock(cache_mutex);
		std::lock_guard<std::mutex> file_lock(file_cache_mutex);
		if (before_time.has_value()) {
			bool any_purged = false;
			for (auto it = sticker_sets_cache.begin(); it != sticker_sets_cache.end();) {
				if (it->second.last_accessed < before_time.value()) {
					spdlog::debug("Purging sticker set '{}' from cache.", it->first);
					it = sticker_sets_cache.erase(it);
					any_purged = true;
				}
				else {
					++it;
				}
			}
			for (auto it = sticker_files_cache.begin(); it != sticker_files_cache.end();) {
				if (it->second.last_accessed < before_time.value()) {
					spdlog::debug("Purging sticker file '{}' from cache.", it->first);
					it = sticker_files_cache.erase(it);
					any_purged = true;
				}
				else {
					++it;
				}
			}
			return any_purged;
		} else {
			size_t count = sticker_sets_cache.size() + sticker_files_cache.size();
			sticker_sets_cache.clear();
			sticker_files_cache.clear();
			spdlog::debug("Purged all sticker sets and files from cache. Total purged: {}", count);
			return count > 0;
		}
	}

	static std::optional<std::expected<nlohmann::json, ErrorResponse>> unwrapTelegramBody(const std::string body) {
		nlohmann::json response_json;
		try {
			response_json = nlohmann::json::parse(body);
		} catch (const std::exception& e) {
			spdlog::error("Failed to parse Telegram API response. Error: {}. Response: {}", e.what(), body);
			return std::nullopt;
		}
		if (!response_json.contains("ok")) {
			spdlog::error("Telegram API response does not contain 'ok' field. Response: {}", body);
			return std::unexpected(ErrorResponse(HTTP_BAD_REQUEST, "Invalid Telegram API response"));
		}
		bool ok = response_json.value("ok", false);
		if (!ok) {
			spdlog::warn("Telegram API response not ok. Body: {}", body);
			try {
				ErrorResponse error_response = nlohmann::json::parse(body).get<ErrorResponse>();
				return std::unexpected(error_response);
			}
			catch (const std::exception& e) {
				spdlog::error("Failed to parse error response from Telegram API. Error: {}. Response: {}", e.what(), body);

			}
		}
		return response_json["result"];
	}

	std::expected<GetStickerSetResponse, ErrorResponse> getStickerSet(const std::string& sticker_set_name) {
		if (sticker_set_name.empty()) {
			spdlog::error("Sticker set name is empty.");
			return std::unexpected(ErrorResponse(HTTP_BAD_REQUEST, "Sticker set name is empty."));
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
			spdlog::warn("Rate limit in effect. Rejecting request for sticker set: {}", sticker_set_name);
			return std::unexpected(ErrorResponse(HTTP_TOO_MANY_REQUESTS, "Rate limit in effect."));
		}

		// Call Telegram API to get sticker set information
		auto response = cli.Get(make_api_url(token, "getStickerSet?name=" + sticker_set_name));

		/*
		 * Check if the response is valid.
		 *
		 * Telegram specifies:
		 * 400 Bad Request: STICKERSET_INVALID: No sticker set exists with the given name,
		 *                  or the name is spelled wrong or has the wrong case.
		 * 400 Bad Request: invalid sticker set name: The provided name contains illegal
		 *                  characters (must be alphanumeric and underscores, ending with _by_<botname>).
		 * 401 Unauthorized: The bot token provided in the request header is invalid or has been revoked.
		 * 429 Too Many Requests: Rate limit or flood control hit; check the retry_after parameter in the
		 *                        response body to see how long to wait before trying again
		 */

		if (!response) {
			spdlog::error("Failed to get response from Telegram API. Error code: {}", static_cast<int>(response.error()));
			return std::unexpected(ErrorResponse(HTTP_INTERNAL_SERVER_ERROR, "Failed to get response from Telegram API"));
		}
		spdlog::debug("Telegram API response status: {}", response->status);
		auto body_opt = unwrapTelegramBody(response->body);
		if (!body_opt) {
			spdlog::error("Failed to unwrap Telegram API response body for sticker set '{}'.", sticker_set_name);
			return std::unexpected(ErrorResponse(HTTP_INTERNAL_SERVER_ERROR, "Failed to unwrap Telegram API response body"));
		}
		auto body = body_opt.value();

		if (response->status == HTTP_OK) {
			spdlog::debug("Successfully retrieved sticker set '{}' from Telegram API.", sticker_set_name);
			spdlog::debug("Telegram API response body: {}", body->dump());

			// Not found in cache, parse the response from Telegram API
			auto sticker_set_opt = GetStickerSetResponse::parse(*body);
			if (!sticker_set_opt) {
				return std::unexpected(ErrorResponse(HTTP_INTERNAL_SERVER_ERROR, "Failed to parse sticker set information from Telegram API"));
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
		return std::unexpected(http_code_err_handle(body.error(), fmt::format("getStickerSet for '{}'", sticker_set_name)));
	}

	void append_ratelimit(int retry_after) {
		std::lock_guard<std::mutex> lock(ratelimit_mutex);
		if (ratelimited_until.has_value()) {
			ratelimited_until = std::max(ratelimited_until.value(), std::chrono::system_clock::now() + std::chrono::seconds(retry_after));
		}
		else {
			ratelimited_until = std::chrono::system_clock::now() + std::chrono::seconds(retry_after);
		}
	}

	bool is_ratelimited() {
		std::lock_guard<std::mutex> lock(ratelimit_mutex);
		return ratelimited_until.has_value() && std::chrono::system_clock::now() < ratelimited_until.value();
	}

	ErrorResponse http_code_err_handle(const ErrorResponse& resp, const std::string_view desc) {
		switch (resp.error_code) {
		case HTTP_OK: {
			return ErrorResponse(resp.error_code, "Success"); // Should not reach
		}
		case HTTP_TOO_MANY_REQUESTS: {
			append_ratelimit(resp.parameters.retry_after.value_or(0));
			[[fallthrough]];
		}
		default: {
			spdlog::error("Telegram API server error for {}. Status code: {}", desc, resp.error_code);
			// Filter request fault and server fault codes, and return the appropriate HTTP status code.
			switch (resp.error_code) {
			case HTTP_BAD_REQUEST:
			case HTTP_NOT_FOUND:
			case HTTP_TOO_MANY_REQUESTS:
				return ErrorResponse(resp.error_code, "Telegram API error.");
			default:
				return ErrorResponse(HTTP_INTERNAL_SERVER_ERROR, "Internal server error.");
			}
		}
		}
	}


	std::expected<Sticker::Data, ErrorResponse> getStickerFile(const std::string& file_id) {
		if (file_id.empty()) {
			spdlog::error("Sticker file_id is empty.");
			return std::unexpected(ErrorResponse(HTTP_BAD_REQUEST, "Sticker file_id is empty."));
		}
		spdlog::debug("Received request for sticker file with file_id: {}", file_id);
		{
			std::lock_guard<std::mutex> lock(file_cache_mutex);
			auto it = sticker_files_cache.find(file_id);
			if (it != sticker_files_cache.end()) {
				spdlog::debug("Sticker file with file_id '{}' found in cache.", file_id);
				it->second.last_accessed = std::chrono::system_clock::now();
				return it->second.data;
			}
		}

		// Check if the Telegram API server is currently rate limited
		if (is_ratelimited()) {
			spdlog::warn("Rate limit in effect. Rejecting request for sticker file with file_id: {}", file_id);
			return std::unexpected(ErrorResponse(HTTP_TOO_MANY_REQUESTS, "Rate limit in effect."));
		}

		auto response = cli.Get(make_api_url(token, "getFile?file_id=" + file_id));
		if (!response) {
			spdlog::error("Failed to get response from Telegram API for sticker file. Error code: {}", static_cast<int>(response.error()));
			return std::unexpected(ErrorResponse(HTTP_INTERNAL_SERVER_ERROR, "Failed to get response from Telegram API."));
		}
		auto body_opt = unwrapTelegramBody(response->body);
		if (!body_opt) {
			spdlog::error("Failed to unwrap Telegram API response body for sticker file with file_id '{}'.", file_id);
			return std::unexpected(ErrorResponse(HTTP_INTERNAL_SERVER_ERROR, "Failed to unwrap Telegram API response body"));
		}
		auto body = body_opt.value();

		spdlog::debug("Telegram API response status for sticker file: {}", response->status);
		if (response->status == HTTP_OK) {
			spdlog::debug("Successfully retrieved sticker file with file_id '{}' from Telegram API.", file_id);
			std::string file_path = (*body)["file_path"];
			if (file_path.empty()) {
				spdlog::error("Telegram API response for sticker file with file_id '{}' does not contain a valid file_path.", file_id);
				return std::unexpected(ErrorResponse(HTTP_INTERNAL_SERVER_ERROR, "Failed to retrieve sticker file path from Telegram API."));
			}
			// Now download the actual file using the file_path
			auto file_response = cli.Get(fmt::format("/file/bot{}/{}", token, file_path));
			if (!file_response) {
				spdlog::error("Failed to download sticker file with file_id '{}' from Telegram API. Error code: {}", file_id, static_cast<int>(file_response.error()));
				return std::unexpected(ErrorResponse(HTTP_INTERNAL_SERVER_ERROR, "Failed to download sticker file from Telegram API."));
			}
			if (file_response->status != HTTP_OK) {
				return std::unexpected(http_code_err_handle(ErrorResponse(file_response->status, "Failed to download sticker file from Telegram API."), fmt::format("download sticker file with file_id '{}'", file_id)));
			}
			spdlog::debug("Successfully downloaded sticker file with file_id '{}' from Telegram API.", file_id);
			// Cache the sticker file binary data for future requests
			{
				std::lock_guard<std::mutex> lock(file_cache_mutex);
				sticker_files_cache[file_id] = Sticker::Data(file_response->body);
			}
			return Sticker::Data(file_response->body);
		}
		return std::unexpected(http_code_err_handle(body.error(), fmt::format("getFile for sticker file with file_id '{}'", file_id)));
	}

	bool listen(std::string ipaddr, const int port) {
		// Try a quick #getMe, testing both the token and the API server URL, before starting the server.
		auto getMeRes = cli.Post(make_api_url(token, "getMe"), "", "application/json");
		if (!getMeRes || getMeRes->status != HTTP_OK) {
			spdlog::error("Failed to get response from Telegram API for getMe");
			if (getMeRes) {
				spdlog::error("Telegram API response status: {}", getMeRes->status);
			}
			return false;
		}

		// Thumbnail image get
		svr.Get("/set/(.*)/(.*)/thumbnail/", [&](const httplib::Request& req, httplib::Response& res) {
			std::string sticker_set_name = req.matches[1];
			std::string sticker_id = req.matches[2];

			spdlog::debug("Received thumbnail request for sticker set: '{}', sticker id: '{}'", sticker_set_name, sticker_id);

			auto sticker_set_result = getStickerSet(sticker_set_name);
			if (!sticker_set_result) {
				// Set not found? Return the error code from getStickerSet
				res.status = sticker_set_result.error().error_code;
				return;
			}
			else {
				auto sticker_set = sticker_set_result.value();
				auto sticker_file_it = std::ranges::find_if(sticker_set.stickers, [&](const Sticker& sticker) {
					return sticker.id == sticker_id;
					});
				if (sticker_file_it == sticker_set.stickers.end()) {
					spdlog::warn("Sticker with id '{}' not found in sticker set '{}'.", sticker_id, sticker_set_name);
					res.status = HTTP_NOT_FOUND;
					return;
				}
				if (!sticker_file_it->thumb_file_id) {
					spdlog::warn("This sticker does not have thumbnail");
					res.status = HTTP_NOT_FOUND;
					return;
				}
				auto sticker_file_result = getStickerFile(*sticker_file_it->thumb_file_id);
				if (!sticker_file_result) {
					res.status = sticker_file_result.error().error_code;
					return;
				}
				else {
					auto sticker_data = sticker_file_result.value();
					res.set_content(sticker_data.data, sticker_data.mimeType());
				}
			}
			});

		// Sticker image get
		svr.Get("/set/(.*)/(.*)/", [&](const httplib::Request& req, httplib::Response& res) {
			std::string sticker_set_name = req.matches[1];
			std::string sticker_id = req.matches[2];

			spdlog::debug("Received request for sticker set: '{}', sticker id: '{}'", sticker_set_name, sticker_id);

			auto sticker_set_result = getStickerSet(sticker_set_name);
			if (!sticker_set_result) {
				// Set not found? Return the error code from getStickerSet
				res.status = sticker_set_result.error().error_code;
				return;
			} else {
				auto sticker_set = sticker_set_result.value();
				auto sticker_file_it = std::ranges::find_if(sticker_set.stickers, [&](const Sticker& sticker) {
					return sticker.id == sticker_id;
				});
				if (sticker_file_it == sticker_set.stickers.end()) {
					spdlog::warn("Sticker with id '{}' not found in sticker set '{}'.", sticker_id, sticker_set_name);
					res.status = HTTP_NOT_FOUND;
					return;
				}
				auto sticker_file_result = getStickerFile(sticker_file_it->file_id);
				if (!sticker_file_result) {
					res.status = sticker_file_result.error().error_code;
					return;
				}
				else {
					auto sticker_data = sticker_file_result.value();
					res.set_content(sticker_data.data, sticker_data.mimeType());
				}
			}
		});

		// Sticker set get
		svr.Get("/set/(.*)/", [&](const httplib::Request& req, httplib::Response& res) {
			std::string sticker_set_name = req.matches[1];
			auto sticker_set_result = getStickerSet(sticker_set_name);
			if (!sticker_set_result) {
				res.status = sticker_set_result.error().error_code;
			} else {
				res.set_content(nlohmann::json(sticker_set_result.value()).dump(), "application/json");
			}
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
void signal_handler(int signal) {
	running.store(false);
}
#endif

int main(int argc, char* argv[]) {
	cxxopts::Options options("StickersFTW BotServer", "A simple HTTP server for serving Telegram stickers.");
	options.add_options()
		("p,port", "Port to run the server on", cxxopts::value<int>()->default_value("8080"))
		("h,host", "Host to run the server on", cxxopts::value<std::string>()->default_value("localhost"))
		("t,token", "Telegram bot token", cxxopts::value<std::string>())
		("l,log-level", "Log level (debug, info, warning, error, critical)", cxxopts::value<std::string>()->default_value("info"))
		("s,server", "Telegram API server URL", cxxopts::value<std::string>())
		("help", "Print help");

	cxxopts::ParseResult result;
	
	try {
		result = options.parse(argc, argv);
	}
	catch (const cxxopts::exceptions::exception& e) {
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
	}
	else if (log_level == "info") {
		spdlog::set_level(spdlog::level::info);
	}
	else if (log_level == "warning") {
		spdlog::set_level(spdlog::level::warn);
	}
	else if (log_level == "error") {
		spdlog::set_level(spdlog::level::err);
	}
	else if (log_level == "critical") {
		spdlog::set_level(spdlog::level::critical);
	}
	else {
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
		while (running || !stop_token_signal.stop_requested()) {
			std::unique_lock<std::mutex> lock(stop_mutex_signal);
			stop_cv_signal.wait_for(lock, stop_token_signal, std::chrono::seconds(1), [stop_token_signal] { return stop_token_signal.stop_requested(); });
		}
		spdlog::info("Signal received, shutting down server...");
		engine.shutdown();
	});
	std::jthread cache_purger([&]() {
		while (running || !stop_token_cache.stop_requested()) {
			std::unique_lock<std::mutex> lock(stop_mutex_cache);
			if (stop_cv_cache.wait_for(lock, stop_token_cache, std::chrono::days(1), [stop_token_cache] { return stop_token_cache.stop_requested(); })) {
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