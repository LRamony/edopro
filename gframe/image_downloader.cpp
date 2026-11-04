#include "image_downloader.h"
#include <cerrno>
#include <array>
#include "logging.h"
#include "utils.h"
#include "game_config.h"
#include "file_stream.h"
#include "fmt.h"
#include "curl.h"
#include "data_manager.h"
#include "yugipedia_client.h"

namespace ygo {

struct curl_payload {
	FILE* stream;
	size_t header_written;
	std::array<uint8_t, 8> header;
};

ImageDownloader::ImageDownloader() : stop_threads(false), yugipedia_client(std::make_unique<YugipediaClient>()) {
	download_threads.reserve(gGameConfig->imageDownloadThreads);
	for(int i = 0; i < gGameConfig->imageDownloadThreads; ++i)
		download_threads.emplace_back(&ImageDownloader::DownloadPic, this);
}
ImageDownloader::~ImageDownloader() {
	{
		std::lock_guard<epro::mutex> lck(pic_download);
		stop_threads = true;
		cv.notify_all();
	}
	for(auto& thread : download_threads)
		thread.join();
}
void ImageDownloader::AddDownloadResource(PicSource src) {
	pic_urls.push_back(src);
}

static constexpr std::array<uint8_t, 8> pngheader{ 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };
static constexpr std::array<uint8_t, 3> jpgheader{ 0xff, 0xd8, 0xff };

enum headerType : uint8_t {
	UNK_FILE,
	PNG_FILE,
	JPG_FILE,
};

static headerType ImageHeaderType(const std::array<uint8_t, 8>& header) {
	if(pngheader == header)
		return PNG_FILE;
	if(!memcmp(jpgheader.data(), header.data(), jpgheader.size()))
		return JPG_FILE;
	return UNK_FILE;
}
static size_t write_data(char *ptr, size_t size, size_t nmemb, void *userdata) {
	static constexpr auto header_size = sizeof(curl_payload::header);
	auto data = static_cast<curl_payload*>(userdata);
	const size_t nbytes = size * nmemb;
	if(data->header_written < header_size) {
		auto increase = std::min(nbytes, header_size - data->header_written);
		memcpy(&data->header[data->header_written], ptr, increase);
		data->header_written += increase;
		if(data->header_written == header_size && ImageHeaderType(data->header) == UNK_FILE)
			return 0xffffffff;
	}
	FILE* out = data->stream;
	fwrite(ptr, 1, nbytes, out);
	return nbytes;
}
static epro::path_stringview GetExtension(const std::array<uint8_t, 8>& header) {
	switch(ImageHeaderType(header)) {
		case PNG_FILE:
			return EPRO_TEXT(".png");
		case JPG_FILE:
			return EPRO_TEXT(".jpg");
		default:
			return EPRO_TEXT("");
	}
}
void ImageDownloader::DownloadPic() {
	Utils::SetThreadName("PicDownloader");
	auto curl = curl_easy_init();
	if(curl == nullptr) {
		ErrorLog("Failed to start downloader thread");
		return;
	}
	curl_payload payload;
	char curl_error_buffer[CURL_ERROR_SIZE];
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error_buffer);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &payload);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "EDOPro/41.0.2");
	if(gGameConfig->ssl_certificate_path.size() && Utils::FileExists(Utils::ToPathString(gGameConfig->ssl_certificate_path)))
		curl_easy_setopt(curl, CURLOPT_CAINFO, gGameConfig->ssl_certificate_path.data());
	auto SetPayloadAndUrl = [&payload, &curl](epro::stringview url, FILE* stream) {
		payload.stream = stream;
		payload.header_written = 0;
		curl_easy_setopt(curl, CURLOPT_URL, url.data());
	};
	while(true) {
		std::unique_lock<epro::mutex> lck(pic_download);
		if(stop_threads) {
			curl_easy_cleanup(curl);
			return;
		}
		while(to_download.empty()) {
			cv.wait(lck);
			if(stop_threads) {
				curl_easy_cleanup(curl);
				return;
			}
		}
		auto file = std::move(to_download.front());
		to_download.pop_front();
		auto type = file.type;
		auto code = file.code;
		auto& map_elem = downloading_images[type][code];
		map_elem.status = downloadStatus::DOWNLOADING;
		lck.unlock();
		auto name = epro::format(EPRO_TEXT("./pics/temp/{}"), code);
		if(type == imgType::THUMB)
			type = imgType::ART;
		epro::path_stringview dest;
		switch(type) {
			case imgType::ART:
			case imgType::THUMB: {
				dest = EPRO_TEXT("./pics/{}"sv);
				break;
			}
			case imgType::FIELD: {
				dest = EPRO_TEXT("./pics/field/{}"sv);
				name.append(EPRO_TEXT("_f"));
				break;
			}
			case imgType::COVER: {
				dest = EPRO_TEXT("./pics/cover/{}"sv);
				name.append(EPRO_TEXT("_c"));
				break;
			}
		}
		auto dest_folder = epro::format(dest, code);
		CURLcode res{ static_cast<CURLcode>(1) };
		for(auto& src : pic_urls) {
			if(src.type != type)
				continue;
			auto fp = fileopen(name.data(), "wb");
			if(fp == nullptr) {
				if(gGameConfig->logDownloadErrors) {
					ygo::ErrorLog("Failed opening {} for write.", Utils::ToUTF8IfNeeded(name));
					ygo::ErrorLog("Error: {}.", strerror(errno));
				}
				continue;
			}
			SetPayloadAndUrl(epro::format(src.url, code), fp);
			res = curl_easy_perform(curl);
			fclose(fp);
			if(res == CURLE_OK) {
				const auto ext = GetExtension(payload.header);
				auto image_path = dest_folder;
				image_path.append(ext.data(), ext.size());
				if(Utils::FileMove(name, image_path)) {
					dest_folder = std::move(image_path);
					break;
				}
				res = CURLE_WRITE_ERROR;
				Utils::FileDelete(name);
				continue;
			}
			if(gGameConfig->logDownloadErrors) {
				ygo::ErrorLog("Failed downloading pic for {}", code);
				ygo::ErrorLog("Curl error: ({}) {} ({})", res, curl_easy_strerror(res), curl_error_buffer);
			}
			Utils::FileDelete(name);
		}
		if(res != CURLE_OK && type == imgType::ART && gDataManager) {
			const auto card = gDataManager->cards.find(code);
			if(card != gDataManager->cards.end()) {
				const auto base_name = Utils::ToUTF8IfNeeded(card->second._strings.name);
				const auto localized_name = Utils::ToUTF8IfNeeded(gDataManager->GetName(code));
				const auto download_from_yugipedia = [&](const std::string& card_name) {
					const auto url = yugipedia_client->FetchCardImageUrl(card_name);
					if(!url) {
						if(gGameConfig->logDownloadErrors)
							ygo::ErrorLog("No Yugipedia image found for card {} ({})", code, card_name);
						return false;
					}
					auto fp = fileopen(name.data(), "wb");
					if(fp == nullptr) {
						res = CURLE_WRITE_ERROR;
						if(gGameConfig->logDownloadErrors) {
							ygo::ErrorLog("Failed opening {} for write.", Utils::ToUTF8IfNeeded(name));
							ygo::ErrorLog("Error: {}.", strerror(errno));
						}
						return false;
					}
					SetPayloadAndUrl(*url, fp);
					res = curl_easy_perform(curl);
					fclose(fp);
					if(res == CURLE_OK) {
						auto image_path = dest_folder;
						const auto ext = GetExtension(payload.header);
						image_path.append(ext.data(), ext.size());
						if(Utils::FileMove(name, image_path)) {
							dest_folder = std::move(image_path);
							return true;
						}
						res = CURLE_WRITE_ERROR;
					}
					if(gGameConfig->logDownloadErrors) {
						ygo::ErrorLog("Failed downloading Yugipedia pic for {}", code);
						ygo::ErrorLog("Curl error: ({}) {} ({})", res, curl_easy_strerror(res), curl_error_buffer);
					}
					Utils::FileDelete(name);
					return false;
				};
				if((base_name.empty() || !download_from_yugipedia(base_name)) &&
				   !localized_name.empty() && localized_name != base_name)
					download_from_yugipedia(localized_name);
			}
		}
		lck.lock();
		if(res == CURLE_OK) {
			map_elem.status = downloadStatus::DOWNLOADED;
			map_elem.path = std::move(dest_folder);
		} else
			map_elem.status = downloadStatus::DOWNLOAD_ERROR;
	}
}
void ImageDownloader::AddToDownloadQueue(uint32_t code, imgType type) {
	if(type == imgType::THUMB)
		type = imgType::ART;
	std::lock_guard<epro::mutex> lck(pic_download);
	if(stop_threads)
		return;
	auto& map = downloading_images[type];
	if(map.find(code) == map.end()) {
		map[code].status = downloadStatus::DOWNLOADING;
		to_download.emplace_back(downloadParam{ code, type });
		cv.notify_one();
	}
}
ImageDownloader::downloadStatus ImageDownloader::GetDownloadStatus(uint32_t code, imgType type) {
	if(type == imgType::THUMB)
		type = imgType::ART;
	std::lock_guard<epro::mutex> lk(pic_download);
	auto it = downloading_images[type].find(code);
	if(it == downloading_images[type].end())
		return downloadStatus::NONE;
	return it->second.status;
}
epro::path_stringview ImageDownloader::GetDownloadPath(uint32_t code, imgType type) {
	if(type == imgType::THUMB)
		type = imgType::ART;
	std::lock_guard<epro::mutex> lk(pic_download);
	auto it = downloading_images[type].find(code);
	if(it == downloading_images[type].end())
		return EPRO_TEXT("");
	return it->second.path;
}
}
