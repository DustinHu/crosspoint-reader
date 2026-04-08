#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

// Plain-function-pointer callbacks for Dictionary::lookup.
// Zero overhead: no heap allocation, no vtable, no std::function bloat.
struct DictLookupCallbacks {
  void* ctx = nullptr;
  void (*onProgress)(void* ctx, int percent) = nullptr;
  bool (*shouldCancel)(void* ctx) = nullptr;
};

// ---------------------------------------------------------------------------
// Secondary index (.idx.cp) — generated from StarDict .idx for O(log n) binary search.
// NOTE: Do NOT use __attribute__((packed)) — ESP32-C3 RISC-V faults on unaligned access.
// These structs are naturally aligned (char[] + uint32_t).
// ---------------------------------------------------------------------------
static constexpr int DICT_WORD_MAX = 32;
static constexpr uint32_t SD_INDEX_MAGIC = 0x43504958;  // "CPIX"
static constexpr uint32_t SD_INDEX_VERSION = 1;
static constexpr int SD_INDEX_HEADER_SIZE = 16;
static constexpr int SD_INDEX_ENTRY_SIZE = 44;

struct SdIndexHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t idxFileSize;  // StarDict .idx file size (for invalidation)
  uint32_t entryCount;
};
static_assert(sizeof(SdIndexHeader) == SD_INDEX_HEADER_SIZE,
              "SdIndexHeader size mismatch — update SD_INDEX_HEADER_SIZE");

struct SdIndexEntry {
  char word[DICT_WORD_MAX];  // 32 bytes, truncated for binary search comparison
  uint32_t dictOffset;       // Byte offset into .dict (converted from BE on generation)
  uint32_t dictSize;         // Byte size of definition in .dict
  uint32_t idxWordOffset;    // Byte offset of full word in StarDict .idx (for >31 char verification)
};
static_assert(sizeof(SdIndexEntry) == SD_INDEX_ENTRY_SIZE, "SdIndexEntry size mismatch — update SD_INDEX_ENTRY_SIZE");

// Metadata parsed from a StarDict .ifo file.
struct DictInfo {
  char bookname[128] = "";
  char website[128] = "";
  char date[32] = "";
  char description[256] = "";
  char sametypesequence[16] = "";
  uint32_t wordcount = 0;
  uint32_t altFormCount = 0;
  uint32_t idxfilesize = 0;
  bool hasAltForms = false;
  bool isCompressed = false;  // .dict.dz present but no .dict
  char lang[32] = "";         // e.g. "en-en", "el-el"
  bool valid = false;
};

// Result of an index search — file location of a definition without reading it.
struct DictLocation {
  std::string folderPath;  // dictionary base path (e.g. /dictionary/dict-en-en/dict-data)
  uint32_t offset = 0;     // byte offset in .dict file
  uint32_t size = 0;       // byte length in .dict file
  bool found = false;
};

class Dictionary {
 public:
  static constexpr unsigned long LONG_PRESS_MS = 600;

  // Returns the active dictionary folder base path by reading dictionary.bin from the SD card.
  // If cachePath is non-null and non-empty, reads <cachePath>/dictionary.bin (per-book override).
  // Otherwise reads /.crosspoint/dictionary.bin (global setting).
  // Returns empty string if no dictionary is configured or the file cannot be read.
  static std::string readDictPath(const char* cachePath = nullptr);

  // Writes folderPath to /.crosspoint/dictionary.bin (global setting).
  // Pass empty string to clear the global dictionary.
  static void saveGlobalDictPath(const char* folderPath);

  // Returns true if a dictionary is configured and all required files exist.
  static bool exists(const char* cachePath = nullptr);

  // Returns true if a .syn file exists for the active dictionary.
  // Gates all alternate-form UI — checked at runtime against the physical file.
  static bool hasAltForms(const char* cachePath = nullptr);

  // Validates the dictionary path stored in /.crosspoint/dictionary.bin against the SD card.
  // If the path is missing or the required files are gone, clears the file. Returns true if valid.
  static bool isValidDictionary();

  // Parse the .ifo file in folderPath and return metadata.
  // Also checks for .syn and .dict.dz presence.
  static DictInfo readInfo(const char* folderPath);

  // Search .idx for word (via .idx.oft if present). Returns file location without reading content.
  static DictLocation locate(const std::string& word, const DictLookupCallbacks& cbs = {},
                             const char* cachePath = nullptr);

  // Look up word in .idx (via .idx.oft if present). Returns definition or empty string.
  static std::string lookup(const std::string& word, const DictLookupCallbacks& cbs = {},
                            const char* cachePath = nullptr);

  // Look up word in .syn (via .syn.oft if present).
  // Returns the canonical headword from .idx, or empty string if not found.
  static std::string resolveAltForm(const std::string& word, const char* cachePath = nullptr);

  static std::string cleanWord(const std::string& word);
  static std::vector<std::string> getStemVariants(const std::string& word);

  // Returns up to maxResults words from .idx that are close in edit distance to word.
  // Requires .idx to be accessible; uses .idx.oft if present for neighbourhood search.
  static std::vector<std::string> findSimilar(const std::string& word, int maxResults, const char* cachePath = nullptr);

  // Generate secondary index (.idx.cp) from StarDict .idx for O(log n) binary search.
  // outCorrupt is set to true if the .idx file has structural problems.
  // progressCb is called periodically with (ctx, bytesProcessed, totalBytes).
  // cancelCb returns true to abort generation.
  static bool generateIndex(const char* idxPath, const char* cpIdxPath, bool& outCorrupt,
                            void* ctx = nullptr,
                            void (*progressCb)(void*, size_t, size_t) = nullptr,
                            bool (*cancelCb)(void*) = nullptr);

 private:
  // Shared word read buffer. Lookup functions are single-threaded; this avoids
  // putting a 256-byte array on the stack in every caller (and 512B peak when nested).
  static char wordBuf[256];

  // Read a null-terminated word from an open file into buf (max bufSize-1 chars).
  // Returns the number of characters read (excluding null), or -1 on error.
  static int readWordInto(FsFile& file, char* buf, size_t bufSize);

  // Read the word at ordinal `ordinal` in .idx.
  // folderPath is the dictionary base path (e.g. /dictionary/dict-en-en/dict-data).
  static std::string wordAtOrdinal(const std::string& folderPath, uint32_t ordinal);

  static std::string readDefinition(const std::string& folderPath, uint32_t offset, uint32_t size);

  // Binary search .idx.cp to find the entry matching word. Returns index or -1.
  static int32_t binarySearchIndex(FsFile& cpIdxFile, uint32_t entryCount, const char* word);

  // Binary search .oft to find the page boundary bytes in src containing target.
  // On return, *startByte and *endByte delimit the 32-word page to scan linearly.
  // srcFileSize is used as the upper bound when the page is the last one.
  // Used by findSimilar() and resolveAltForm() which still use OFT files.
  static void findPageBounds(FsFile& oft, FsFile& src, uint32_t srcFileSize, const char* target, uint32_t* startByte,
                             uint32_t* endByte);

  static int editDistance(const std::string& a, const std::string& b, int maxDist);
};
