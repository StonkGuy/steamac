import Darwin
import Foundation

/// Filesystem-specific checks and publication for the SteamOS disk creator.
enum DiskCreationFilesystem {
    /// An expected rejection of the chosen location/capacity or busy cache, not a launcher defect.
    struct Rejection: Error, CustomStringConvertible {
        let description: String
        init(_ description: String) { self.description = description }
    }

    /// Before downloading: reject volumes that cannot hold the disk. Returns whether files
    /// occupy their full logical size (exFAT does not support sparse files).
    static func preflight(directory: String, diskBytes: UInt64, rootfsBytes: UInt64, existingBytes: UInt64 = 0) throws -> Bool {
        var fs = statfs()
        guard statfs(directory, &fs) == 0 else {
            throw OptionError("\(directory): \(String(cString: strerror(errno)))")
        }
        guard fs.f_flags & UInt32(MNT_RDONLY) == 0 else {
            throw Rejection("\(directory) is on a read-only volume: choose a writable location for the SteamOS disk")
        }
        let type = withUnsafePointer(to: &fs.f_fstypename) {
            $0.withMemoryRebound(to: CChar.self, capacity: Int(MFSTYPENAMELEN)) { String(cString: $0) }
        }
        guard type != "msdos" || max(diskBytes, rootfsBytes) <= UInt64(UInt32.max) else {
            throw Rejection("\(directory) is on a FAT32/MS-DOS volume, which cannot store files of 4 GiB or larger. "
                + "The SteamOS disk and its 10 GiB rootfs exceed this limit: choose an APFS, Mac OS Extended, or exFAT volume")
        }
        let dense = type == "exfat"
        if dense {
            let need = diskBytes - min(existingBytes, diskBytes) + rootfsBytes + (1 << 30)
            let free = UInt64(fs.f_bavail) * UInt64(fs.f_bsize)
            guard free >= need else {
                throw Rejection(String(format: "not enough free space on exFAT: %.1f GB needed on %@, %.1f GB free. "
                    + "exFAT stores the full disk size (no sparse files); choose a smaller home size or an APFS volume",
                    Double(need) / 1e9, directory, Double(free) / 1e9))
            }
        }
        return dense
    }

    /// The caller must hold creation.lock until publication is complete.
    static func publish(partial: String, destination: String) throws {
        if renamex_np(partial, destination, UInt32(RENAME_EXCL)) == 0 { return }
        let error = errno
        if error == EEXIST {
            throw Rejection("\(destination) already exists (never overwritten; delete it or choose another path)")
        }
        guard error == ENOTSUP || error == EINVAL else {
            throw OptionError("rename \(partial) -> \(destination): \(String(cString: strerror(error)))")
        }
        // exFAT does not implement RENAME_EXCL. All launcher creators serialize on
        // creation.lock; lstat also rejects dangling symlinks. The check + ordinary rename
        // is not atomic against non-launcher programs creating the destination concurrently.
        var existing = stat()
        if lstat(destination, &existing) == 0 {
            throw Rejection("\(destination) already exists (never overwritten; delete it or choose another path)")
        }
        let statError = errno
        guard statError == ENOENT else {
            throw OptionError("\(destination): \(String(cString: strerror(statError)))")
        }
        guard rename(partial, destination) == 0 else {
            throw OptionError("rename \(partial) -> \(destination): \(String(cString: strerror(errno)))")
        }
    }
}
