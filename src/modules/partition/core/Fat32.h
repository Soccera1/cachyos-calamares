/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PARTITION_FAT32_H
#define PARTITION_FAT32_H

#include <cstddef>
#include <cstdint>

namespace PartUtils::Fat32
{
constexpr std::uint64_t minimumClusters = 65527;
constexpr std::uint64_t specificationMinimumClusters = 65525;

// Cluster count, rather than the filesystem label, determines the FAT variant.
inline bool isBelowSpecificationMinimum( std::int64_t clusters )
{
    return clusters >= 0 && std::uint64_t( clusters ) < specificationMinimumClusters;
}

inline bool isBelowRecommendedMinimum( std::int64_t clusters )
{
    return clusters >= 0 && std::uint64_t( clusters ) >= specificationMinimumClusters
        && std::uint64_t( clusters ) < minimumClusters;
}

// A newly formatted small FAT32 volume uses one sector per cluster, 32 reserved
// sectors and two FATs. Include the FAT entries for reserved clusters 0 and 1.
inline std::uint64_t minimumVolumeSize( std::uint64_t sectorSize )
{
    if ( sectorSize < 512 || sectorSize > 4096 || ( sectorSize & ( sectorSize - 1 ) ) )
    {
        return 0;
    }
    const auto fatSectors = ( ( minimumClusters + 2 ) * 4 + sectorSize - 1 ) / sectorSize;
    return ( 32 + 2 * fatSectors + minimumClusters ) * sectorSize;
}

// Read the BPB rather than deriving a cluster count from the partition size:
// existing filesystems can have larger clusters or occupy less than the partition.
inline std::uint64_t clusterCount( const unsigned char* boot, std::size_t size, std::uint64_t capacity )
{
    if ( size < 512 || boot[510] != 0x55 || boot[511] != 0xaa )
    {
        return 0;
    }
    const auto word = [ boot ]( int offset ) -> std::uint64_t
    { return boot[offset] | ( std::uint64_t( boot[offset + 1] ) << 8 ); };
    const auto dword = [ &word ]( int offset ) { return word( offset ) | ( word( offset + 2 ) << 16 ); };
    const auto sectorSize = word( 11 );
    const auto sectorsPerCluster = boot[13];
    const auto reserved = word( 14 );
    const auto fats = boot[16];
    const auto fatSectors = dword( 36 );
    const auto totalSectors = word( 19 ) ? word( 19 ) : dword( 32 );
    if ( !minimumVolumeSize( sectorSize ) || !sectorsPerCluster || sectorsPerCluster > 128
         || ( sectorsPerCluster & ( sectorsPerCluster - 1 ) ) || !reserved || !fats || !fatSectors
         || word( 17 ) || word( 22 ) || word( 42 ) || totalSectors > capacity / sectorSize )
    {
        return 0;
    }
    const auto overhead = reserved + fats * fatSectors;
    if ( totalSectors <= overhead )
    {
        return 0;
    }
    const auto clusters = ( totalSectors - overhead ) / sectorsPerCluster;
    return clusters + 2 <= fatSectors * sectorSize / 4 ? clusters : 0;
}
}  // namespace PartUtils::Fat32
#endif
