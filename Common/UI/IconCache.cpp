#include "Common/UI/IconCache.h"
#include "Common/UI/Context.h"
#include "Common/TimeUtil.h"
#include "Common/Data/Format/PNGLoad.h"
#include "Common/Log.h"

#define ICON_CACHE_VERSION 1

#define MK_FOURCC(str) (str[0] | ((uint8_t)str[1] << 8) | ((uint8_t)str[2] << 16) | ((uint8_t)str[3] << 24))

const uint32_t ICON_CACHE_MAGIC = MK_FOURCC("pICN");

IconCache g_iconCache;

struct DiskCacheHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t entryCount;
};

struct DiskCacheEntry {
	uint32_t keyLen;
	uint32_t dataLen;
	IconFormat format;
	double insertedTimestamp;
};

void IconCache::SaveToFile(FILE *file) {
	Decimate();

	std::unique_lock<std::mutex> lock(lock_);

	DiskCacheHeader header;
	header.magic = ICON_CACHE_MAGIC;
	header.version = ICON_CACHE_VERSION;
	header.entryCount = 0;  // (uint32_t)cache_.size();

	fwrite(&header, 1, sizeof(header), file);

	return;

	for (auto &iter : cache_) {
		DiskCacheEntry entryHeader;
		entryHeader.keyLen = (uint32_t)iter.first.size();
		entryHeader.dataLen = (uint32_t)iter.second.data.size();
		entryHeader.format = iter.second.format;
		entryHeader.insertedTimestamp = iter.second.insertedTimeStamp;
		fwrite(&entryHeader, 1, sizeof(entryHeader), file);
		fwrite(iter.first.c_str(), 1, iter.first.size(), file);
		fwrite(iter.second.data.data(), 1, iter.second.data.size(), file);
	}
}

bool IconCache::LoadFromFile(FILE *file) {
	std::unique_lock<std::mutex> lock(lock_);

	DiskCacheHeader header;
	if (fread(&header, 1, sizeof(header), file) != sizeof(DiskCacheHeader)) {
		return false;
	}
	if (header.magic != ICON_CACHE_MAGIC || header.version != ICON_CACHE_VERSION) {
		return false;
	}

	double now = time_now_d();

	for (uint32_t i = 0; i < header.entryCount; i++) {
		DiskCacheEntry entryHeader;
		if (fread(&entryHeader, 1, sizeof(entryHeader), file) != sizeof(entryHeader)) {
			break;
		}

		std::string key;
		key.resize(entryHeader.keyLen, 0);
		if (entryHeader.keyLen > 0x1000) {
			// Let's say this is invalid, probably a corrupted file.
			break;
		}

		fread(&key[0], 1, entryHeader.keyLen, file);

		// Check if we already have the entry somehow.
		if (cache_.find(key) != cache_.end()) {
			// Seek past the data and go to the next entry.
			fseek(file, entryHeader.dataLen, SEEK_CUR);
			continue;
		}

		std::string data;
		data.resize(entryHeader.dataLen);
		fread(&data[0], 1, entryHeader.dataLen, file);

		Entry entry{};
		entry.data = data;
		entry.format = entryHeader.format;
		entry.insertedTimeStamp = entryHeader.insertedTimestamp;
		entry.usedTimeStamp = now;
		cache_.insert(std::pair<std::string, Entry>(key, entry));
	}

	return true;
}

void IconCache::ClearTextures() {
	std::unique_lock<std::mutex> lock(lock_);
	for (auto &iter : cache_) {
		iter.second.texture->Release();
	}
}

void IconCache::ClearData() {
	ClearTextures();
	std::unique_lock<std::mutex> lock(lock_);
	cache_.clear();
}

void IconCache::Decimate() {
	std::unique_lock<std::mutex> lock(lock_);

}

bool IconCache::GetDimensions(const std::string &key, int *width, int *height) {
	std::unique_lock<std::mutex> lock(lock_);
	auto iter = cache_.find(key);
	if (iter == cache_.end()) {
		// Don't have this entry.
		return false;
	}

	if (iter->second.texture) {
		// TODO: Store the width/height in the cache.
		*width = iter->second.texture->Width();
		*height = iter->second.texture->Height();
		return true;
	} else {
		return false;
	}
}

bool IconCache::Contains(const std::string &key) {
	std::unique_lock<std::mutex> lock(lock_);
	return cache_.find(key) != cache_.end();
}

bool IconCache::InsertIcon(const std::string &key, IconFormat format, std::string &&data) {
	std::unique_lock<std::mutex> lock(lock_);

	if (key.empty()) {
		return false;
	}

	if (data.empty()) {
		ERROR_LOG(G3D, "Can't insert empty data into icon cache");
		return false;
	}

	if (cache_.find(key) != cache_.end()) {
		// Already have this entry.
		return false;
	}

	double now = time_now_d();
	cache_.emplace(key, Entry{ std::move(data), format, nullptr, now, now, false });
	return true;
}

Draw::Texture *IconCache::BindIconTexture(UIContext *context, const std::string &key) {
	std::unique_lock<std::mutex> lock(lock_);
	auto iter = cache_.find(key);
	if (iter == cache_.end()) {
		// Don't have this entry.
		return nullptr;
	}

	if (iter->second.texture) {
		context->GetDrawContext()->BindTexture(0, iter->second.texture);
		iter->second.usedTimeStamp = time_now_d();
		return iter->second.texture;
	}

	if (iter->second.badData) {
		return nullptr;
	}

	// OK, don't have a texture. Upload it!
	int width = 0;
	int height = 0;
	Draw::DataFormat dataFormat;
	unsigned char *buffer = nullptr;

	switch (iter->second.format) {
	case IconFormat::PNG:
	{
		int result = pngLoadPtr((const unsigned char *)iter->second.data.data(), iter->second.data.size(), &width,
			&height, &buffer);

		if (result != 1) {
			ERROR_LOG(G3D, "IconCache: Failed to load png (%d bytes) for key %s", (int)iter->second.data.size(), key.c_str());
			iter->second.badData = true;
			return nullptr;
		}
		dataFormat = Draw::DataFormat::R8G8B8A8_UNORM;
		break;
	}
	default:
		return nullptr;
	}

	Draw::TextureDesc iconDesc{};
	iconDesc.width = width;
	iconDesc.height = height;
	iconDesc.depth = 1;
	iconDesc.initData.push_back((const uint8_t *)buffer);
	iconDesc.mipLevels = 1;
	iconDesc.swizzle = Draw::TextureSwizzle::DEFAULT;
	iconDesc.generateMips = false;
	iconDesc.tag = key.c_str();
	iconDesc.format = dataFormat;
	iconDesc.type = Draw::TextureType::LINEAR2D;

	Draw::Texture *texture = context->GetDrawContext()->CreateTexture(iconDesc);
	iter->second.texture = texture;

	free(buffer);

	return texture;
}

IconCacheStats IconCache::GetStats() {
	IconCacheStats stats{};

	std::unique_lock<std::mutex> lock(lock_);

	for (auto &iter : cache_) {
		stats.cachedCount++;
		if (iter.second.texture)
			stats.textureCount++;
		stats.dataSize += iter.second.data.size();
	}

	return stats;
}
