/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "core/Fat32.h"

#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QtTest/QtTest>

#include <array>

using namespace PartUtils::Fat32;

class Fat32Tests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void clusterBoundary();
    void warningBoundaries();
    void invalidBootSector();
    void formattedVolumes();
};

static std::array< unsigned char, 512 >
bootSector( unsigned clusters, unsigned sectorSize, unsigned sectorsPerCluster )
{
    std::array< unsigned char, 512 > boot {};
    const auto put = [ &boot ]( int offset, unsigned value, int bytes )
    {
        for ( int i = 0; i < bytes; ++i )
        {
            boot[offset + i] = value >> ( i * 8 );
        }
    };
    const auto fatSectors = ( ( clusters + 2 ) * 4 + sectorSize - 1 ) / sectorSize;
    put( 11, sectorSize, 2 );
    put( 13, sectorsPerCluster, 1 );
    put( 14, 32, 2 );
    put( 16, 2, 1 );
    put( 32, 32 + 2 * fatSectors + clusters * sectorsPerCluster, 4 );
    put( 36, fatSectors, 4 );
    put( 510, 0xaa55, 2 );
    return boot;
}

void
Fat32Tests::clusterBoundary()
{
    for ( unsigned sectorSize : { 512, 4096 } )
    {
        for ( unsigned sectorsPerCluster : { 1, 8 } )
        {
            for ( unsigned clusters : { 4084, 4085, 65524, 65525, 65526, 65527, 65528 } )
            {
                const auto boot = bootSector( clusters, sectorSize, sectorsPerCluster );
                QCOMPARE( clusterCount( boot.data(), boot.size(), 8ULL * 1024 * 1024 * 1024 ), clusters );
                QCOMPARE( clusterCount( boot.data(), boot.size(), 8ULL * 1024 * 1024 * 1024 ) >= minimumClusters,
                          clusters >= 65527 );
            }
        }
    }
    // Raw data capacity alone is insufficient: FAT tables and reserved sectors need space.
    QVERIFY( minimumVolumeSize( 512 ) > minimumClusters * 512 );
    QVERIFY( minimumVolumeSize( 4096 ) > minimumClusters * 4096 );
    // Automatic GRUB creation and the manual-format minimum use these sizes.
    QCOMPARE( ( minimumVolumeSize( 512 ) + 1048575 ) / 1048576, 33ULL );
    QCOMPARE( ( minimumVolumeSize( 4096 ) + 1048575 ) / 1048576, 257ULL );
    QCOMPARE( minimumVolumeSize( 0 ), 0ULL );
    QCOMPARE( minimumVolumeSize( 513 ), 0ULL );
}

void
Fat32Tests::warningBoundaries()
{
    for ( qint64 clusters : { -1, 0, 4084, 4085, 65524, 65525, 65526, 65527, 65528 } )
    {
        QCOMPARE( isBelowSpecificationMinimum( clusters ), clusters >= 0 && clusters < 65525 );
        QCOMPARE( isBelowRecommendedMinimum( clusters ), clusters == 65525 || clusters == 65526 );
    }
}

void
Fat32Tests::invalidBootSector()
{
    auto boot = bootSector( 65527, 512, 1 );
    constexpr auto capacity = 1024ULL * 1024 * 1024;
    QCOMPARE( clusterCount( boot.data(), 511, capacity ), 0ULL );
    QCOMPARE( clusterCount( boot.data(), boot.size(), 32 * 1024 * 1024 ), 0ULL );
    boot[13] = 0;
    QCOMPARE( clusterCount( boot.data(), boot.size(), capacity ), 0ULL );
    boot[13] = 3;
    QCOMPARE( clusterCount( boot.data(), boot.size(), capacity ), 0ULL );
    boot[13] = 1;
    boot[22] = 1;  // FAT16 layout
    QCOMPARE( clusterCount( boot.data(), boot.size(), capacity ), 0ULL );
    boot[22] = 0;
    boot[36] = 1;  // FAT too short to hold the data clusters
    boot[37] = 0;
    QCOMPARE( clusterCount( boot.data(), boot.size(), capacity ), 0ULL );
    boot[510] = 0;
    QCOMPARE( clusterCount( boot.data(), boot.size(), capacity ), 0ULL );
}

void
Fat32Tests::formattedVolumes()
{
    const auto mkfs = QStandardPaths::findExecutable( QStringLiteral( "mkfs.fat" ) );
    if ( mkfs.isEmpty() )
    {
        QSKIP( "mkfs.fat is not available" );
    }
    for ( unsigned sectorSize : { 512, 4096 } )
    {
        // Verify real formatter output just below and above the minimum rounded to MiB.
        const auto minimum = minimumVolumeSize( sectorSize );
        const auto minimumMiB = ( minimum + 1048575 ) / 1048576;
        for ( auto sizeMiB : { minimumMiB - 1, minimumMiB } )
        {
            QTemporaryFile image;
            QVERIFY( image.open() );
            QVERIFY( image.resize( sizeMiB * 1048576 ) );
            QProcess process;
            process.start( mkfs, { "-F", "32", "-S", QString::number( sectorSize ), image.fileName() } );
            QVERIFY( process.waitForFinished() );
            QCOMPARE( process.exitCode(), 0 );
            const auto boot = image.read( 512 );
            const auto clusters = clusterCount( reinterpret_cast< const unsigned char* >( boot.constData() ),
                                               boot.size(), image.size() );
            QCOMPARE( clusters >= minimumClusters, sizeMiB >= minimumMiB );
        }
    }
}

QTEST_GUILESS_MAIN( Fat32Tests )
#include "Fat32Tests.moc"
