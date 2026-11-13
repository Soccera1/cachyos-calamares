/* === This file is part of Calamares - <https://calamares.io> ===
 *
 *   SPDX-FileCopyrightText: 2015-2016 Teo Mrnjavac <teo@kde.org>
 *   SPDX-FileCopyrightText: 2018-2019 Adriaan de Groot <groot@kde.org>
 *   SPDX-FileCopyrightText: 2019 Collabora Ltd <arnaud.ferraris@collabora.com>
 *   SPDX-License-Identifier: GPL-3.0-or-later
 *
 *   Calamares is Free Software: see the License-Identifier above.
 *
 */

#include "PartUtils.h"

#include "core/DeviceModel.h"
#include "core/Fat32.h"
#include "core/KPMHelpers.h"
#include "core/PartitionInfo.h"

#include "GlobalStorage.h"
#include "JobQueue.h"
#include "partition/Mount.h"
#include "partition/PartitionIterator.h"
#include "partition/PartitionQuery.h"
#include "utils/Logger.h"
#include "utils/RAII.h"
#include "utils/System.h"

#include <kpmcore/backend/corebackend.h>
#include <kpmcore/backend/corebackendmanager.h>
#include <kpmcore/core/device.h>
#include <kpmcore/core/partition.h>

#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

using Calamares::Partition::isPartitionFreeSpace;
using Calamares::Partition::isPartitionNew;

using Calamares::Units::operator""_MiB;

static constexpr qint64 efiSpecificationHardMinimumSize = 32_MiB;

namespace PartUtils
{

QString
convenienceName( const Partition* const candidate )
{
    if ( !candidate->mountPoint().isEmpty() )
    {
        return candidate->mountPoint();
    }
    if ( !candidate->partitionPath().isEmpty() )
    {
        return candidate->partitionPath();
    }
    if ( !candidate->devicePath().isEmpty() )
    {
        return candidate->devicePath();
    }
    if ( !candidate->deviceNode().isEmpty() )
    {
        return candidate->devicePath();
    }

    QString p;
    QTextStream s( &p );
    s << static_cast< const void* >( candidate );  // No good name available, use pointer address

    return p;
}

/** @brief Get the globalStorage setting for required space. */
static double
getRequiredStorageGiB( bool& ok )
{
    return Calamares::JobQueue::instance()->globalStorage()->value( "requiredStorageGiB" ).toDouble( &ok );
}

bool
canBeReplaced( Partition* candidate, const Logger::Once& o )
{
    if ( !candidate )
    {
        cDebug() << o << "Partition* is NULL";
        return false;
    }

    cDebug() << o << "Checking if" << convenienceName( candidate ) << "can be replaced.";
    if ( candidate->isMounted() )
    {
        cDebug() << Logger::SubEntry << "NO, it is mounted.";
        return false;
    }

    bool ok = false;
    double requiredStorageGiB = getRequiredStorageGiB( ok );
    if ( !ok )
    {
        cDebug() << Logger::SubEntry << "NO, requiredStorageGiB is not set correctly.";
        return false;
    }

    qint64 availableStorageB = candidate->capacity();
    qint64 requiredStorageB = Calamares::GiBtoBytes( requiredStorageGiB + 0.5 );

    if ( availableStorageB > requiredStorageB )
    {
        cDebug() << o << "Partition" << convenienceName( candidate ) << "authorized for replace install.";
        return true;
    }
    else
    {
        Logger::CDebug deb;
        deb << Logger::SubEntry << "NO, insufficient storage";
        deb << Logger::Continuation << "Required  storage B:" << requiredStorageB
            << QString( "(%1GiB)" ).arg( requiredStorageGiB );
        deb << Logger::Continuation << "Available storage B:" << availableStorageB
            << QString( "(%1GiB)" ).arg( Calamares::BytesToGiB( availableStorageB ) );
        return false;
    }
}

bool
canBeResized( Partition* candidate, const Logger::Once& o )
{
    if ( !candidate )
    {
        cDebug() << o << "Partition* is NULL";
        return false;
    }

    if ( !candidate->fileSystem().supportGrow() || !candidate->fileSystem().supportShrink() )
    {
        cDebug() << o << "Can not resize" << convenienceName( candidate ) << ", filesystem"
                 << candidate->fileSystem().name() << "does not support resize.";
        return false;
    }

    if ( isPartitionFreeSpace( candidate ) )
    {
        cDebug() << o << "Can not resize" << convenienceName( candidate ) << ", partition is free space";
        return false;
    }

    if ( candidate->isMounted() )
    {
        cDebug() << o << "Can not resize" << convenienceName( candidate ) << ", partition is mounted";
        return false;
    }

    if ( candidate->roles().has( PartitionRole::Primary ) )
    {
        PartitionTable* table = dynamic_cast< PartitionTable* >( candidate->parent() );
        if ( !table )
        {
            cDebug() << o << "Can not resize" << convenienceName( candidate ) << ", no partition table found";
            return false;
        }

        if ( table->numPrimaries() >= table->maxPrimaries() )
        {
            cDebug() << o << "Can not resize" << convenienceName( candidate ) << ", partition table already has"
                     << table->maxPrimaries() << "primary partitions.";
            return false;
        }
    }

    bool ok = false;
    double requiredStorageGiB = getRequiredStorageGiB( ok );
    if ( !ok )
    {
        cDebug() << o << "Can not resize" << convenienceName( candidate )
                 << ", requiredStorageGiB is not set correctly.";
        return false;
    }

    // We require a little more for partitioning overhead and swap file
    double advisedStorageGiB = requiredStorageGiB + 0.5 + 2.0;
    qint64 availableStorageB = candidate->available();
    qint64 advisedStorageB = Calamares::GiBtoBytes( advisedStorageGiB );

    if ( availableStorageB > advisedStorageB )
    {
        cDebug() << o << "Partition" << convenienceName( candidate )
                 << "authorized for resize + autopartition install.";
        return true;
    }
    else
    {
        Logger::CDebug deb;
        deb << Logger::SubEntry << "NO, insufficient storage";
        deb << Logger::Continuation << "Required  storage B:" << advisedStorageB
            << QString( "(%1GiB)" ).arg( advisedStorageGiB );
        deb << Logger::Continuation << "Available storage B:" << availableStorageB
            << QString( "(%1GiB)" ).arg( Calamares::BytesToGiB( availableStorageB ) ) << "for"
            << convenienceName( candidate ) << "length:" << candidate->length()
            << "sectorsUsed:" << candidate->sectorsUsed() << "fsType:" << candidate->fileSystem().name();
        return false;
    }
}

bool
canBeResized( DeviceModel* dm, const QString& partitionPath, const Logger::Once& o )
{
    if ( partitionPath.startsWith( "/dev/" ) )
    {
        for ( int i = 0; i < dm->rowCount(); ++i )
        {
            Device* dev = dm->deviceForIndex( dm->index( i ) );
            Partition* candidate = Calamares::Partition::findPartitionByPath( { dev }, partitionPath );
            if ( candidate )
            {
                return canBeResized( candidate, o );
            }
        }
        cWarning() << "Can not resize" << partitionPath << ", no Partition* found.";
        return false;
    }
    else
    {
        cWarning() << "Can not resize" << partitionPath << ", does not start with /dev";
        return false;
    }
}

static FstabEntryList
lookForFstabEntries( const QString& partitionPath )
{
    QStringList mountOptions { "ro" };

    auto r = Calamares::System::runCommand( Calamares::System::RunLocation::RunInHost,
                                            { "blkid", "-s", "TYPE", "-o", "value", partitionPath } );
    if ( r.getExitCode() )
    {
        cWarning() << "blkid on" << partitionPath << "failed.";
    }
    else
    {
        QString fstype = r.getOutput().trimmed();
        if ( ( fstype == "ext3" ) || ( fstype == "ext4" ) )
        {
            mountOptions.append( "noload" );
        }
    }

    cDebug() << "Checking device" << partitionPath << "for fstab (fs=" << r.getOutput() << ')';

    Calamares::Partition::TemporaryMount mount( partitionPath, QString(), mountOptions.join( ',' ) );
    if ( mount.isValid() )
    {
        QFile fstabFile( mount.path() + "/etc/fstab" );

        if ( fstabFile.open( QIODevice::ReadOnly | QIODevice::Text ) )
        {
            const auto fstabLines = QString::fromLocal8Bit( fstabFile.readAll() ).split( '\n' );
            fstabFile.close();

            const auto fstabEntries = Calamares::fromEtcFstabContents( fstabLines );
            cDebug() << Logger::SubEntry << "got" << fstabEntries.count() << "fstab entries from" << fstabLines.count()
                     << "lines in" << fstabFile.fileName();
            return fstabEntries;
        }
        else
        {
            cWarning() << "Could not read fstab from mounted fs";
            return {};
        }
    }
    else
    {
        cWarning() << "Could not mount existing fs";
        return {};
    }
}

static QString
findPartitionPathForMountPoint( const FstabEntryList& fstab, const QString& mountPoint )
{
    if ( fstab.isEmpty() )
    {
        return QString();
    }

    for ( const FstabEntry& entry : fstab )
    {
        if ( entry.mountPoint == mountPoint )
        {
            QProcess readlink;
            QString partPath;

            if ( entry.partitionNode.startsWith( "/dev" ) )  // plain dev node
            {
                partPath = entry.partitionNode;
            }
            else if ( entry.partitionNode.startsWith( "LABEL=" ) )
            {
                partPath = entry.partitionNode.mid( 6 );
                partPath.remove( "\"" );
                partPath.replace( "\\040", "\\ " );
                partPath.prepend( "/dev/disk/by-label/" );
            }
            else if ( entry.partitionNode.startsWith( "UUID=" ) )
            {
                partPath = entry.partitionNode.mid( 5 );
                partPath.remove( "\"" );
                partPath = partPath.toLower();
                partPath.prepend( "/dev/disk/by-uuid/" );
            }
            else if ( entry.partitionNode.startsWith( "PARTLABEL=" ) )
            {
                partPath = entry.partitionNode.mid( 10 );
                partPath.remove( "\"" );
                partPath.replace( "\\040", "\\ " );
                partPath.prepend( "/dev/disk/by-partlabel/" );
            }
            else if ( entry.partitionNode.startsWith( "PARTUUID=" ) )
            {
                partPath = entry.partitionNode.mid( 9 );
                partPath.remove( "\"" );
                partPath = partPath.toLower();
                partPath.prepend( "/dev/disk/by-partuuid/" );
            }

            // At this point we either have /dev/sda1, or /dev/disk/by-something/...

            if ( partPath.startsWith( "/dev/disk/by-" ) )  // we got a fancy node
            {
                readlink.start( "readlink", { "-en", partPath } );
                if ( !readlink.waitForStarted( 1000 ) )
                {
                    return QString();
                }
                if ( !readlink.waitForFinished( 1000 ) )
                {
                    return QString();
                }
                if ( readlink.exitCode() != 0 || readlink.exitStatus() != QProcess::NormalExit )
                {
                    return QString();
                }
                partPath = QString::fromLocal8Bit( readlink.readAllStandardOutput() ).trimmed();
            }

            return partPath;
        }
    }

    return QString();
}

OsproberEntryList
runOsprober( DeviceModel* dm )
{
    Logger::Once o;

    QString osproberOutput;
    QProcess osprober;
    osprober.setProgram( "os-prober" );
    osprober.setProcessChannelMode( QProcess::SeparateChannels );
    osprober.start();
    if ( !osprober.waitForStarted() )
    {
        cError() << "os-prober cannot start.";
    }
    else if ( !osprober.waitForFinished( 60000 ) )
    {
        cError() << "os-prober timed out.";
    }
    else
    {
        osproberOutput.append( QString::fromLocal8Bit( osprober.readAllStandardOutput() ).trimmed() );
    }

    QStringList osproberCleanLines;
    OsproberEntryList osproberEntries;
    const auto lines = osproberOutput.split( '\n' );
    for ( const QString& line : lines )
    {
        if ( !line.simplified().isEmpty() )
        {
            QStringList lineColumns = line.split( ':' );
            QString prettyName;
            if ( !lineColumns.value( 1 ).simplified().isEmpty() )
            {
                prettyName = lineColumns.value( 1 ).simplified();
            }
            else if ( !lineColumns.value( 2 ).simplified().isEmpty() )
            {
                prettyName = lineColumns.value( 2 ).simplified();
            }

            QString file, path = lineColumns.value( 0 ).simplified();
            if ( !path.startsWith( "/dev/" ) )  //basic sanity check
            {
                continue;
            }

            // strip extra file after device: /dev/name@/path/to/file
            int index = path.indexOf( '@' );
            if ( index != -1 )
            {
                file = path.right( path.length() - index - 1 );
                path = path.left( index );
            }

            FstabEntryList fstabEntries = lookForFstabEntries( path );
            QString homePath = findPartitionPathForMountPoint( fstabEntries, "/home" );

            osproberEntries.append( { prettyName,
                                      path,
                                      file,
                                      QString(),
                                      canBeResized( dm, path, o ),
                                      lineColumns,
                                      fstabEntries,
                                      homePath } );
            osproberCleanLines.append( line );
        }
    }

    if ( osproberCleanLines.count() > 0 )
    {
        cDebug() << o << "os-prober lines after cleanup:" << Logger::DebugList( osproberCleanLines );
    }
    else
    {
        cDebug() << o << "os-prober gave no output.";
    }

    Calamares::JobQueue::instance()->globalStorage()->insert( "osproberLines", osproberCleanLines );

    return osproberEntries;
}

bool
isArmSystem()
{
    Calamares::GlobalStorage* gs = Calamares::JobQueue::instance()->globalStorage();
    return gs->contains( "armInstall" ) && gs->value( "armInstall" ).toBool();
}

bool
isEfiSystem()
{
    return isArmSystem() || QDir( "/sys/firmware/efi/efivars" ).exists();
}

bool
isEfiFilesystemSuitableType( const Partition* candidate )
{
    auto type = candidate->fileSystem().type();

    QT_WARNING_PUSH
    QT_WARNING_DISABLE_CLANG( "-Wswitch-enum" )
    switch ( type )
    {
    case FileSystem::Type::Fat32:
        return true;
    case FileSystem::Type::Fat12:
    case FileSystem::Type::Fat16:
        cWarning() << "FAT12 and FAT16 are probably not supported by EFI";
        return false;
    default:
        cWarning() << "EFI boot partition must be FAT32";
        return false;
    }
    QT_WARNING_POP
}

bool
isEfiFilesystemClusterBased()
{
    const auto* gs = Calamares::JobQueue::instance()->globalStorage();
    return gs->value( "packagechooser_bootloader" ).toString() == QLatin1String( "grub" )
        && gs->value( "efiSystemPartition" ).toString() == QLatin1String( "/boot/efi" );
}

qint64
efiFilesystemMinimumSizeMiB( const Partition* candidate, qint64 logicalSectorSize )
{
    if ( !isEfiFilesystemClusterBased() )
    {
        return ( efiFilesystemMinimumSize() + 1_MiB - 1 ) / 1_MiB;
    }

    const auto sectorSize = candidate ? candidate->sectorSize() : logicalSectorSize;
    auto minimum = Fat32::minimumVolumeSize( sectorSize );
    if ( candidate && !isPartitionNew( candidate ) && !PartitionInfo::format( candidate ) )
    {
        QFile device( candidate->partitionPath() );
        if ( device.open( QIODevice::ReadOnly ) )
        {
            const auto boot = device.read( 512 );
            const auto* data = reinterpret_cast< const unsigned char* >( boot.constData() );
            if ( Fat32::clusterCount( data, boot.size(), candidate->capacity() ) )
            {
                const auto word = [ data ]( int offset ) -> quint64
                { return data[offset] | ( quint64( data[offset + 1] ) << 8 ); };
                const auto dword = [ &word ]( int offset ) { return word( offset ) | ( word( offset + 2 ) << 16 ); };
                const auto bytesPerSector = word( 11 );
                const auto fatSectors = std::max< quint64 >(
                    dword( 36 ), ( ( Fat32::minimumClusters + 2 ) * 4 + bytesPerSector - 1 ) / bytesPerSector );
                minimum = ( word( 14 ) + data[16] * fatSectors + Fat32::minimumClusters * data[13] )
                    * bytesPerSector;
            }
        }
    }
    return ( minimum + 1_MiB - 1 ) / 1_MiB;
}

qint64
efiFilesystemClusterCount( const Partition* candidate )
{
    if ( !candidate || candidate->capacity() <= 0 || !isEfiFilesystemSuitableType( candidate ) )
    {
        return -1;
    }
    if ( isPartitionNew( candidate ) || PartitionInfo::format( candidate ) )
    {
        // Small mkfs.fat FAT32 volumes use one sector per cluster, two FATs
        // and 32 reserved sectors. Recalculate after rounding FAT lengths.
        const quint64 sectorSize = candidate->sectorSize();
        if ( !Fat32::minimumVolumeSize( sectorSize ) || candidate->capacity() / sectorSize <= 32 )
        {
            return -1;
        }
        const quint64 fatAndDataSectors = candidate->capacity() / sectorSize - 32;
        const auto clusters = ( fatAndDataSectors * sectorSize + 16 ) / ( sectorSize + 8 );
        const auto fatSectors = ( ( clusters + 2 ) * 4 + sectorSize - 1 ) / sectorSize;
        return fatAndDataSectors > 2 * fatSectors ? qint64( fatAndDataSectors - 2 * fatSectors ) : 0;
    }
    QFile device( candidate->partitionPath() );
    if ( !device.open( QIODevice::ReadOnly ) )
    {
        return -1;
    }
    const auto boot = device.read( 512 );
    const auto clusters = Fat32::clusterCount( reinterpret_cast< const unsigned char* >( boot.constData() ),
                                              boot.size(), candidate->capacity() );
    return clusters ? qint64( clusters ) : -1;
}

static bool
hasMinimumEfiClusters( const Partition* candidate )
{
    if ( !isEfiFilesystemSuitableType( candidate ) || candidate->capacity() <= 0 )
    {
        return false;
    }
    if ( isPartitionNew( candidate ) || PartitionInfo::format( candidate ) )
    {
        const auto minimum = Fat32::minimumVolumeSize( candidate->sectorSize() );
        // Leave room for mkfs.fat geometry rounding as well as FAT metadata.
        const auto alignedMinimum = ( minimum + 1_MiB - 1 ) / 1_MiB * 1_MiB;
        return minimum && quint64( candidate->capacity() ) >= alignedMinimum;
    }

    QFile device( candidate->partitionPath() );
    if ( !device.open( QIODevice::ReadOnly ) )
    {
        cWarning() << "Cannot read FAT32 cluster count for" << candidate->partitionPath();
        return false;
    }
    const auto boot = device.read( 512 );
    return Fat32::clusterCount( reinterpret_cast< const unsigned char* >( boot.constData() ),
                                boot.size(),
                                candidate->capacity() ) >= Fat32::minimumClusters;
}

bool
isEfiFilesystemRecommendedSize( const Partition* candidate )
{
    if ( isEfiFilesystemClusterBased() )
    {
        return hasMinimumEfiClusters( candidate );
    }

    auto size = candidate->capacity();  // bytes
    if ( size <= 0 )
    {
        return false;
    }

    if ( size >= efiFilesystemRecommendedSize() )
    {
        return true;
    }
    else
    {
        cWarning() << "Filesystem for EFI is smaller than recommended (" << size << "bytes)";
        return false;
    }
}

bool
isEfiFilesystemMinimumSize( const Partition* candidate )
{
    if ( isEfiFilesystemClusterBased() )
    {
        return hasMinimumEfiClusters( candidate );
    }

    using Calamares::Units::operator""_MiB;

    auto size = candidate->capacity();  // bytes
    if ( size <= 0 )
    {
        return false;
    }
    if ( size < efiSpecificationHardMinimumSize )
    {
        return false;
    }

    if ( size >= efiFilesystemMinimumSize() )
    {
        return true;
    }
    else
    {
        cWarning() << "Filesystem for EFI is below minimum (" << size << "bytes)";
        return false;
    }
}

bool
isEfiBootable( const Partition* candidate )
{
    const auto flags = PartitionInfo::flags( candidate );

    // In KPMCore4, the flags are remapped, and the ESP flag is the same as Boot.
    static_assert( KPM_PARTITION_FLAG_ESP == KPM_PARTITION_FLAG( Boot ), "KPMCore API enum changed" );
    return flags.testFlag( KPM_PARTITION_FLAG_ESP );
}

QString
efiFilesystemRecommendedSizeGSKey()
{
    return QStringLiteral( "efiSystemPartitionSize_i" );
}

qint64
efiFilesystemRecommendedSize()
{
    const QString key = efiFilesystemRecommendedSizeGSKey();

    qint64 uefisys_part_sizeB = 300_MiB;

    // The default can be overridden; the key used here comes
    // from the partition module Config.cpp
    auto* gs = Calamares::JobQueue::instance()->globalStorage();
    if ( gs->contains( key ) )
    {
        qint64 v = gs->value( key ).toLongLong();
        uefisys_part_sizeB = v > 0 ? v : 0;
    }
    // There is a lower limit of what can be configured
    if ( uefisys_part_sizeB < efiSpecificationHardMinimumSize )
    {
        uefisys_part_sizeB = efiSpecificationHardMinimumSize;
    }
    return uefisys_part_sizeB;
}

QString
efiFilesystemMinimumSizeGSKey()
{
    return QStringLiteral( "efiSystemPartitionMinimumSize_i" );
}

qint64
efiFilesystemMinimumSize()
{
    const QString key = efiFilesystemMinimumSizeGSKey();

    qint64 uefisys_part_sizeB = efiFilesystemRecommendedSize();

    // The default can be overridden; the key used here comes
    // from the partition module Config.cpp
    auto* gs = Calamares::JobQueue::instance()->globalStorage();
    if ( gs->contains( key ) )
    {
        qint64 v = gs->value( key ).toLongLong();
        uefisys_part_sizeB = v > 0 ? v : 0;
    }
    // There is a lower limit of what can be configured
    return std::max( uefisys_part_sizeB, efiSpecificationHardMinimumSize );
}

QString
canonicalFilesystemName( const QString& fsName, FileSystem::Type* fsType )
{
    cScopedAssignment type( fsType );
    if ( fsName.isEmpty() )
    {
        type = FileSystem::Ext4;
        return QStringLiteral( "ext4" );
    }

    QStringList fsLanguage { QLatin1String( "C" ) };  // Required language list to turn off localization

    if ( ( type = FileSystem::typeForName( fsName, fsLanguage ) ) != FileSystem::Unknown )
    {
        return fsName;
    }

    // Second pass: try case-insensitive
    const auto fstypes = FileSystem::types();
    for ( FileSystem::Type t : fstypes )
    {
        if ( 0 == QString::compare( fsName, FileSystem::nameForType( t, fsLanguage ), Qt::CaseInsensitive ) )
        {
            QString fsRealName = FileSystem::nameForType( t, fsLanguage );
            if ( fsType )
            {
                *fsType = t;
            }
            return fsRealName;
        }
    }

    cWarning() << "Filesystem" << fsName << "not found, using ext4";
    // fsType can be used to check whether fsName was a valid filesystem.
    if ( fsType )
    {
        *fsType = FileSystem::Unknown;
    }
#ifdef DEBUG_FILESYSTEMS
    // This bit is for distros who are debugging their settings, and shows
    // all the strings that KPMCore is matching against for FS type.
    {
        Logger::CDebug d;
        using TR = Logger::DebugRow< int, QString >;
        const auto fstypes = FileSystem::types();
        d << "Available types (" << fstypes.count() << ')';
        for ( FileSystem::Type t : fstypes )
        {
            d << TR( static_cast< int >( t ), FileSystem::nameForType( t, fsLanguage ) );
        }
    }
#endif
    type = FileSystem::Unknown;
    return QStringLiteral( "ext4" );
}

}  // namespace PartUtils
