// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifndef M4SIDECAR_H
#define M4SIDECAR_H

#include "inc/Core/Common.h"
#include "inc/Core/VectorIndex.h"
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace SPTAG
{
namespace M4
{

// Page size constant (4KB)
constexpr size_t PageSize = 4096;
constexpr size_t PageSizeEx = 12;

// M4 Sidecar file format version
constexpr uint32_t M4_FORMAT_VERSION = 1;

// M4 Metadata
struct M4Meta
{
    uint32_t m_formatVersion = M4_FORMAT_VERSION;
    uint32_t m_layoutType = 0; // 0=VIDOrder, 1=PrimaryPostingOrder, 2=CoHitOrder, 3=HotnessOrder
    uint32_t m_totalVIDCount = 0;
    uint32_t m_totalPostingCount = 0;
    uint32_t m_dimension = 0;
    uint32_t m_valueType = 0; // Float=0, Int8=1, Int16=2, UInt8=3
    uint64_t m_payloadStoreSize = 0;
    uint64_t m_routeStoreSize = 0;
    uint64_t m_locTableSize = 0;
    uint32_t m_checksum = 0;
};

// Location table entry: VID -> payload location
struct LocationEntry
{
    int m_vid = -1;
    uint64_t m_payloadOffset = 0; // Byte offset in payload store
    uint32_t m_pageIdx = 0;       // Page index in payload store
    uint16_t m_byteOffset = 0;    // Byte offset within page
    uint16_t m_reserved = 0;
};

// Route posting entry: VID list for a posting
struct RoutePostingHeader
{
    int m_postingID = -1;
    int m_eleCount = 0;
    uint64_t m_routeDataOffset = 0;
};

// M4 Sidecar builder options
struct M4BuilderOptions
{
    std::string m_inputIndexFile;   // Input SPTAGFullList.bin path
    std::string m_outputDir;        // Output directory for sidecar files
    int m_layoutType = 0;           // Layout type for primary payload ordering
    bool m_verbose = true;
};

// M4 Sidecar builder
class M4SidecarBuilder
{
public:
    M4SidecarBuilder(const M4BuilderOptions& p_opts);
    ~M4SidecarBuilder() = default;

    // Build sidecar from legacy index
    bool Build();

    // Get statistics
    size_t GetLegacySize() const { return m_legacySize; }
    size_t GetSidecarSize() const { return m_sidecarSize; }
    double GetStorageReduction() const;

private:
    // Read legacy index header
    bool ReadLegacyHeader();

    // Read all postings and collect VID information
    bool CollectVIDInfo();

    // Assign primary posting for each VID
    void AssignPrimaries();

    // Write route posting files
    bool WriteRoutePostings();

    // Write primary payload store
    bool WritePayloadStore();

    // Write location table
    bool WriteLocationTable();

    // Write metadata file
    bool WriteMeta();

    // Write checksum file
    bool WriteChecksum();

    // Calculate checksum
    uint32_t CalculateChecksum();

private:
    M4BuilderOptions m_opts;

    // Legacy index info
    int m_listCount = 0;
    int m_totalDocumentCount = 0;
    int m_dimension = 0;
    int m_listPageOffset = 0;
    size_t m_legacySize = 0;
    int m_vectorInfoSize = 0;

    // Per-posting info (from legacy)
    struct PostingInfo
    {
        int m_postingID = -1;
        uint64_t m_listOffset = 0;
        int m_eleCount = 0;
        uint16_t m_pageOffset = 0;
        uint16_t m_listPageCount = 0;
        std::vector<int> m_vids; // VIDs in this posting
    };
    std::vector<PostingInfo> m_postings;

    // VID info
    struct VIDInfo
    {
        std::vector<int> m_postingIDs; // Postings containing this VID
        int m_primaryPostingID = -1;   // Assigned primary posting
        uint64_t m_payloadOffset = 0;  // Offset in payload store
        uint32_t m_pageIdx = 0;
        uint16_t m_byteOffset = 0;
    };
    std::unordered_map<int, VIDInfo> m_vidInfo;

    // Statistics
    size_t m_sidecarSize = 0;
    size_t m_payloadStoreSize = 0;
    size_t m_routeStoreSize = 0;
    size_t m_locTableSize = 0;
};

} // namespace M4
} // namespace SPTAG

#endif // M4SIDECAR_H
