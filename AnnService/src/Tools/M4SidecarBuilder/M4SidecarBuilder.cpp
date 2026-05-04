// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

// M4 Sidecar Builder - Build primary-secondary sidecar from legacy SPANN index

#include "inc/Core/Common.h"
#include "inc/Core/SPANN/M4Sidecar.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <set>
#include <sys/stat.h>
#include <unordered_set>

using namespace SPTAG;
using namespace SPTAG::M4;

// FNV-1a hash for checksum
static uint32_t fnv1a_hash(const uint8_t* data, size_t len, uint32_t hash = 2166136261u)
{
    for (size_t i = 0; i < len; ++i)
    {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

// Helper to create directory
static bool create_directory(const std::string& path)
{
    // Use the mkdir macro defined in Common.h
    return mkdir(path.c_str()) == 0 || errno == EEXIST;
}

M4SidecarBuilder::M4SidecarBuilder(const M4BuilderOptions& p_opts)
    : m_opts(p_opts)
{
}

bool M4SidecarBuilder::Build()
{
    auto t1 = std::chrono::high_resolution_clock::now();

    std::cout << "M4 Sidecar Builder" << std::endl;
    std::cout << "==================" << std::endl;
    std::cout << "Input: " << m_opts.m_inputIndexFile << std::endl;
    std::cout << "Output: " << m_opts.m_outputDir << std::endl;
    std::cout << std::endl;

    // Step 1: Read legacy header
    std::cout << "[1/6] Reading legacy index header..." << std::endl;
    if (!ReadLegacyHeader())
    {
        std::cerr << "ERROR: Failed to read legacy header" << std::endl;
        return false;
    }
    std::cout << "  List count: " << m_listCount << std::endl;
    std::cout << "  Total documents: " << m_totalDocumentCount << std::endl;
    std::cout << "  Dimension: " << m_dimension << std::endl;
    std::cout << "  Vector info size: " << m_vectorInfoSize << " bytes" << std::endl;

    // Step 2: Collect VID info from all postings
    std::cout << "[2/6] Collecting VID info from postings..." << std::endl;
    if (!CollectVIDInfo())
    {
        std::cerr << "ERROR: Failed to collect VID info" << std::endl;
        return false;
    }
    std::cout << "  Unique VIDs: " << m_vidInfo.size() << std::endl;

    // Step 3: Assign primary posting for each VID
    std::cout << "[3/6] Assigning primary postings..." << std::endl;
    AssignPrimaries();

    // Step 4: Write route postings
    std::cout << "[4/6] Writing route postings..." << std::endl;
    if (!WriteRoutePostings())
    {
        std::cerr << "ERROR: Failed to write route postings" << std::endl;
        return false;
    }
    std::cout << "  Route store size: " << (m_routeStoreSize >> 20) << " MB" << std::endl;

    // Step 5: Write primary payload store
    std::cout << "[5/6] Writing primary payload store..." << std::endl;
    if (!WritePayloadStore())
    {
        std::cerr << "ERROR: Failed to write payload store" << std::endl;
        return false;
    }
    std::cout << "  Payload store size: " << (m_payloadStoreSize >> 20) << " MB" << std::endl;

    // Step 6: Write location table
    std::cout << "[6/6] Writing location table..." << std::endl;
    if (!WriteLocationTable())
    {
        std::cerr << "ERROR: Failed to write location table" << std::endl;
        return false;
    }
    std::cout << "  Location table size: " << (m_locTableSize >> 20) << " MB" << std::endl;

    // Calculate total sidecar size BEFORE writing meta
    m_sidecarSize = m_payloadStoreSize + m_routeStoreSize + m_locTableSize + 1024; // +1KB for meta

    // Write metadata and checksum
    std::cout << std::endl << "Writing metadata and checksum..." << std::endl;
    if (!WriteMeta())
    {
        std::cerr << "ERROR: Failed to write metadata" << std::endl;
        return false;
    }
    if (!WriteChecksum())
    {
        std::cerr << "ERROR: Failed to write checksum" << std::endl;
        return false;
    }

    // Print summary
    auto t2 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t2 - t1).count();

    std::cout << std::endl;
    std::cout << "Build Complete!" << std::endl;
    std::cout << "===============" << std::endl;
    std::cout << "Legacy size: " << (m_legacySize >> 20) << " MB" << std::endl;
    std::cout << "Sidecar size: " << (m_sidecarSize >> 20) << " MB" << std::endl;
    std::cout << "Storage reduction: " << std::fixed << std::setprecision(1)
              << (GetStorageReduction() * 100) << "%" << std::endl;
    std::cout << "Time elapsed: " << std::fixed << std::setprecision(2) << elapsed << " sec" << std::endl;

    return true;
}

double M4SidecarBuilder::GetStorageReduction() const
{
    if (m_legacySize == 0) return 0.0;
    return 1.0 - static_cast<double>(m_sidecarSize) / static_cast<double>(m_legacySize);
}

bool M4SidecarBuilder::ReadLegacyHeader()
{
    std::ifstream in(m_opts.m_inputIndexFile, std::ios::binary);
    if (!in.is_open())
    {
        std::cerr << "Failed to open file: " << m_opts.m_inputIndexFile << std::endl;
        return false;
    }

    // Read header
    in.read(reinterpret_cast<char*>(&m_listCount), sizeof(m_listCount));
    if (!in)
    {
        std::cerr << "Failed to read list count" << std::endl;
        return false;
    }
    in.read(reinterpret_cast<char*>(&m_totalDocumentCount), sizeof(m_totalDocumentCount));
    if (!in)
    {
        std::cerr << "Failed to read total document count" << std::endl;
        return false;
    }
    in.read(reinterpret_cast<char*>(&m_dimension), sizeof(m_dimension));
    if (!in)
    {
        std::cerr << "Failed to read dimension" << std::endl;
        return false;
    }
    in.read(reinterpret_cast<char*>(&m_listPageOffset), sizeof(m_listPageOffset));
    if (!in)
    {
        std::cerr << "Failed to read list page offset" << std::endl;
        return false;
    }

    // Calculate vector info size (VID + payload)
    // Assuming UInt8 for now (will be configurable later)
    m_vectorInfoSize = m_dimension * sizeof(uint8_t) + sizeof(int);

    // Resize postings vector
    m_postings.resize(m_listCount);

    // Read posting metadata
    for (int i = 0; i < m_listCount; ++i)
    {
        int pageNum = 0;
        uint16_t pageOffset = 0;
        int listEleCount = 0;
        uint16_t listPageCount = 0;

        in.read(reinterpret_cast<char*>(&pageNum), sizeof(pageNum));
        if (!in)
        {
            std::cerr << "Failed to read page num for posting " << i << std::endl;
            return false;
        }
        in.read(reinterpret_cast<char*>(&pageOffset), sizeof(pageOffset));
        if (!in)
        {
            std::cerr << "Failed to read page offset for posting " << i << std::endl;
            return false;
        }
        in.read(reinterpret_cast<char*>(&listEleCount), sizeof(listEleCount));
        if (!in)
        {
            std::cerr << "Failed to read ele count for posting " << i << std::endl;
            return false;
        }
        in.read(reinterpret_cast<char*>(&listPageCount), sizeof(listPageCount));
        if (!in)
        {
            std::cerr << "Failed to read page count for posting " << i << std::endl;
            return false;
        }

        m_postings[i].m_postingID = i;
        m_postings[i].m_listOffset = (static_cast<uint64_t>(m_listPageOffset + pageNum) << PageSizeEx);
        m_postings[i].m_pageOffset = pageOffset;
        m_postings[i].m_eleCount = listEleCount;
        m_postings[i].m_listPageCount = listPageCount;
    }

    // Get file size
    in.seekg(0, std::ios::end);
    m_legacySize = in.tellg();

    in.close();
    return true;
}

bool M4SidecarBuilder::CollectVIDInfo()
{
    std::ifstream in(m_opts.m_inputIndexFile, std::ios::binary);
    if (!in.is_open())
    {
        return false;
    }

    // Buffer for reading record
    std::vector<char> buffer(m_vectorInfoSize);

    size_t totalRecords = 0;

    for (int i = 0; i < m_listCount; ++i)
    {
        if (m_postings[i].m_eleCount == 0)
        {
            continue;
        }

        // Seek to posting position
        in.seekg(m_postings[i].m_listOffset, std::ios::beg);

        // Read all VIDs in this posting
        m_postings[i].m_vids.reserve(m_postings[i].m_eleCount);

        for (int j = 0; j < m_postings[i].m_eleCount; ++j)
        {
            in.read(buffer.data(), m_vectorInfoSize);
            if (!in)
            {
                std::cerr << "Failed to read record " << j << " in posting " << i << std::endl;
                return false;
            }

            // Extract VID (first 4 bytes)
            int vid;
            std::memcpy(&vid, buffer.data(), sizeof(int));

            m_postings[i].m_vids.push_back(vid);
            m_vidInfo[vid].m_postingIDs.push_back(i);

            ++totalRecords;
        }

        if (m_opts.m_verbose && i % 10000 == 0 && i > 0)
        {
            std::cout << "  Processed " << i << "/" << m_listCount << " postings..." << std::endl;
        }
    }

    in.close();

    std::cout << "  Total records: " << totalRecords << std::endl;
    std::cout << "  Unique VIDs: " << m_vidInfo.size() << std::endl;

    // Calculate replica statistics
    size_t totalReplicas = 0;
    size_t maxReplicas = 0;
    for (const auto& kv : m_vidInfo)
    {
        size_t replicas = kv.second.m_postingIDs.size();
        totalReplicas += replicas;
        maxReplicas = std::max(maxReplicas, replicas);
    }
    std::cout << "  Average replicas per VID: " << std::fixed << std::setprecision(2)
              << static_cast<double>(totalReplicas) / m_vidInfo.size() << std::endl;
    std::cout << "  Max replicas: " << maxReplicas << std::endl;

    return true;
}

void M4SidecarBuilder::AssignPrimaries()
{
    // Layout 0: VIDOrder - Primary = first posting (min posting ID)
    // This provides deterministic assignment

    for (auto& kv : m_vidInfo)
    {
        int vid = kv.first;
        auto& info = kv.second;

        if (info.m_postingIDs.empty())
        {
            std::cerr << "WARNING: VID " << vid << " has no postings!" << std::endl;
            continue;
        }

        // Sort posting IDs and pick the smallest as primary
        std::sort(info.m_postingIDs.begin(), info.m_postingIDs.end());
        info.m_primaryPostingID = info.m_postingIDs[0];
    }

    std::cout << "  Assigned primaries using VIDOrder layout" << std::endl;
}

bool M4SidecarBuilder::WriteRoutePostings()
{
    std::string routeFile = m_opts.m_outputDir + "/ssdIndex.m4.route";
    std::string routeDirFile = m_opts.m_outputDir + "/ssdIndex.m4.route.dir";

    // Create output directory if not exists
    create_directory(m_opts.m_outputDir);

    // Write route directory (posting ID -> route data offset)
    std::ofstream dirOut(routeDirFile, std::ios::binary | std::ios::trunc);
    if (!dirOut.is_open())
    {
        std::cerr << "Failed to open route directory file: " << routeDirFile << std::endl;
        return false;
    }

    // Write route data (VID lists)
    std::ofstream routeOut(routeFile, std::ios::binary | std::ios::trunc);
    if (!routeOut.is_open())
    {
        std::cerr << "Failed to open route file: " << routeFile << std::endl;
        return false;
    }

    // Header: number of postings
    dirOut.write(reinterpret_cast<const char*>(&m_listCount), sizeof(int));

    uint64_t currentOffset = 0;

    for (int i = 0; i < m_listCount; ++i)
    {
        // Directory entry: posting ID, element count, data offset
        dirOut.write(reinterpret_cast<const char*>(&i), sizeof(int));
        dirOut.write(reinterpret_cast<const char*>(&m_postings[i].m_eleCount), sizeof(int));
        dirOut.write(reinterpret_cast<const char*>(&currentOffset), sizeof(uint64_t));

        if (m_postings[i].m_eleCount > 0)
        {
            // Route data: just VIDs (4 bytes each)
            for (int vid : m_postings[i].m_vids)
            {
                routeOut.write(reinterpret_cast<const char*>(&vid), sizeof(int));
            }
            currentOffset += m_postings[i].m_eleCount * sizeof(int);
        }
    }

    dirOut.close();
    routeOut.close();

    m_routeStoreSize = currentOffset;
    m_routeStoreSize += m_listCount * (sizeof(int) + sizeof(int) + sizeof(uint64_t)); // Directory size

    return true;
}

bool M4SidecarBuilder::WritePayloadStore()
{
    std::string payloadFile = m_opts.m_outputDir + "/ssdIndex.m4.payload";

    std::ofstream out(payloadFile, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        std::cerr << "Failed to open payload file: " << payloadFile << std::endl;
        return false;
    }

    // Header
    uint32_t magic = 0x4D34504D; // "M4PM"
    out.write(reinterpret_cast<const char*>(&magic), sizeof(uint32_t));
    out.write(reinterpret_cast<const char*>(&m_dimension), sizeof(uint32_t));
    uint32_t valueType = 3; // UInt8
    out.write(reinterpret_cast<const char*>(&valueType), sizeof(uint32_t));
    uint32_t vidCount = static_cast<uint32_t>(m_vidInfo.size());
    out.write(reinterpret_cast<const char*>(&vidCount), sizeof(uint32_t));

    // Open legacy index for reading payloads
    std::ifstream in(m_opts.m_inputIndexFile, std::ios::binary);
    if (!in.is_open())
    {
        std::cerr << "Failed to open legacy index for payload reading" << std::endl;
        return false;
    }

    // Buffer for reading payload
    std::vector<char> buffer(m_vectorInfoSize);

    // Collect and sort VIDs
    std::vector<int> sortedVIDs;
    sortedVIDs.reserve(m_vidInfo.size());
    for (const auto& kv : m_vidInfo)
    {
        sortedVIDs.push_back(kv.first);
    }
    std::sort(sortedVIDs.begin(), sortedVIDs.end());

    // Page tracking
    uint64_t currentOffset = 4 * sizeof(uint32_t); // Header size

    for (int vid : sortedVIDs)
    {
        auto& info = m_vidInfo[vid];

        // Seek to primary posting and read the record
        int primaryPosting = info.m_primaryPostingID;
        bool found = false;

        // Find the VID in the primary posting
        for (int j = 0; j < m_postings[primaryPosting].m_eleCount; ++j)
        {
            if (m_postings[primaryPosting].m_vids[j] == vid)
            {
                uint64_t recordOffset = m_postings[primaryPosting].m_listOffset + j * m_vectorInfoSize;
                in.seekg(recordOffset, std::ios::beg);
                in.read(buffer.data(), m_vectorInfoSize);
                found = true;
                break;
            }
        }

        if (!found)
        {
            std::cerr << "WARNING: VID " << vid << " not found in primary posting " << primaryPosting << std::endl;
            continue;
        }

        // Record payload location
        info.m_payloadOffset = currentOffset;
        info.m_pageIdx = static_cast<uint32_t>((currentOffset - 4 * sizeof(uint32_t)) / PageSize);
        info.m_byteOffset = static_cast<uint16_t>((currentOffset - 4 * sizeof(uint32_t)) % PageSize);

        // Write VID + payload (full record)
        out.write(buffer.data(), m_vectorInfoSize);

        // Update offsets
        currentOffset += m_vectorInfoSize;
    }

    in.close();
    out.close();

    m_payloadStoreSize = currentOffset;

    return true;
}

bool M4SidecarBuilder::WriteLocationTable()
{
    std::string locFile = m_opts.m_outputDir + "/ssdIndex.m4.loc";

    std::ofstream out(locFile, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        std::cerr << "Failed to open location file: " << locFile << std::endl;
        return false;
    }

    // Header
    uint32_t magic = 0x4D344C4F; // "M4LO"
    out.write(reinterpret_cast<const char*>(&magic), sizeof(uint32_t));
    uint32_t entryCount = static_cast<uint32_t>(m_vidInfo.size());
    out.write(reinterpret_cast<const char*>(&entryCount), sizeof(uint32_t));

    // Write location entries in VID order
    std::vector<int> sortedVIDs;
    sortedVIDs.reserve(m_vidInfo.size());
    for (const auto& kv : m_vidInfo)
    {
        sortedVIDs.push_back(kv.first);
    }
    std::sort(sortedVIDs.begin(), sortedVIDs.end());

    for (int vid : sortedVIDs)
    {
        const auto& info = m_vidInfo[vid];

        // Entry: VID, payload offset, page idx, byte offset
        out.write(reinterpret_cast<const char*>(&vid), sizeof(int));
        out.write(reinterpret_cast<const char*>(&info.m_payloadOffset), sizeof(uint64_t));
        out.write(reinterpret_cast<const char*>(&info.m_pageIdx), sizeof(uint32_t));
        out.write(reinterpret_cast<const char*>(&info.m_byteOffset), sizeof(uint16_t));

        // Reserved for future use
        uint16_t reserved = 0;
        out.write(reinterpret_cast<const char*>(&reserved), sizeof(uint16_t));
    }

    out.close();

    m_locTableSize = 2 * sizeof(uint32_t) + m_vidInfo.size() * (sizeof(int) + sizeof(uint64_t) + sizeof(uint32_t) + 2 * sizeof(uint16_t));

    return true;
}

bool M4SidecarBuilder::WriteMeta()
{
    std::string metaFile = m_opts.m_outputDir + "/ssdIndex.m4.meta";

    std::ofstream out(metaFile, std::ios::out | std::ios::trunc);
    if (!out.is_open())
    {
        std::cerr << "Failed to open meta file: " << metaFile << std::endl;
        return false;
    }

    out << "[M4]" << std::endl;
    out << "FormatVersion=" << M4_FORMAT_VERSION << std::endl;
    out << "LayoutType=" << m_opts.m_layoutType << std::endl;
    out << "TotalVIDCount=" << m_vidInfo.size() << std::endl;
    out << "TotalPostingCount=" << m_listCount << std::endl;
    out << "Dimension=" << m_dimension << std::endl;
    out << "ValueType=UInt8" << std::endl;
    out << "PayloadStoreSize=" << m_payloadStoreSize << std::endl;
    out << "RouteStoreSize=" << m_routeStoreSize << std::endl;
    out << "LocTableSize=" << m_locTableSize << std::endl;
    out << "LegacySize=" << m_legacySize << std::endl;
    out << "SidecarSize=" << m_sidecarSize << std::endl;
    out << "StorageReduction=" << std::fixed << std::setprecision(4) << GetStorageReduction() << std::endl;

    out.close();

    return true;
}

bool M4SidecarBuilder::WriteChecksum()
{
    std::string checksumFile = m_opts.m_outputDir + "/ssdIndex.m4.checksum";

    std::ofstream out(checksumFile, std::ios::out | std::ios::trunc);
    if (!out.is_open())
    {
        std::cerr << "Failed to open checksum file: " << checksumFile << std::endl;
        return false;
    }

    uint32_t checksum = CalculateChecksum();

    out << "[Checksum]" << std::endl;
    out << "Value=" << checksum << std::endl;

    out.close();

    return true;
}

uint32_t M4SidecarBuilder::CalculateChecksum()
{
    // Simple checksum based on file sizes and counts
    uint32_t hash = 2166136261u;

    // Include key values in hash
    hash = fnv1a_hash(reinterpret_cast<const uint8_t*>(&m_listCount), sizeof(m_listCount), hash);
    hash = fnv1a_hash(reinterpret_cast<const uint8_t*>(&m_totalDocumentCount), sizeof(m_totalDocumentCount), hash);
    hash = fnv1a_hash(reinterpret_cast<const uint8_t*>(&m_dimension), sizeof(m_dimension), hash);

    uint32_t vidCount = static_cast<uint32_t>(m_vidInfo.size());
    hash = fnv1a_hash(reinterpret_cast<const uint8_t*>(&vidCount), sizeof(vidCount), hash);

    return hash;
}

// Main entry point
void printUsage(const char* progName)
{
    std::cout << "Usage: " << progName << " [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --input <file>     Input SPTAGFullList.bin file" << std::endl;
    std::cout << "  --output <dir>     Output directory for sidecar files" << std::endl;
    std::cout << "  --layout <type>    Layout type (0=VIDOrder, default=0)" << std::endl;
    std::cout << "  --quiet            Suppress verbose output" << std::endl;
    std::cout << "  --help             Show this help" << std::endl;
}

int main(int argc, char* argv[])
{
    M4BuilderOptions opts;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--input" && i + 1 < argc)
        {
            opts.m_inputIndexFile = argv[++i];
        }
        else if (arg == "--output" && i + 1 < argc)
        {
            opts.m_outputDir = argv[++i];
        }
        else if (arg == "--layout" && i + 1 < argc)
        {
            opts.m_layoutType = std::stoi(argv[++i]);
        }
        else if (arg == "--quiet")
        {
            opts.m_verbose = false;
        }
        else if (arg == "--help")
        {
            printUsage(argv[0]);
            return 0;
        }
        else
        {
            std::cerr << "Unknown argument: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }

    if (opts.m_inputIndexFile.empty() || opts.m_outputDir.empty())
    {
        std::cerr << "ERROR: --input and --output are required" << std::endl;
        printUsage(argv[0]);
        return 1;
    }

    M4SidecarBuilder builder(opts);
    if (!builder.Build())
    {
        return 1;
    }

    return 0;
}
