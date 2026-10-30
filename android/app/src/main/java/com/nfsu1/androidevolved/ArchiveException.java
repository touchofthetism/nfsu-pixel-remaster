package com.nfsu1.androidevolved;

import java.io.EOFException;
import java.io.IOException;
import java.util.zip.DataFormatException;
import java.util.zip.ZipException;
import org.tukaani.xz.CorruptedInputException;
import org.tukaani.xz.MemoryLimitException;
import org.tukaani.xz.UnsupportedOptionsException;

/**
 * Why an archive could not be read, in terms the importer can explain (GameImport turns the kind into a message).
 * Free of Android types so the readers can be tested on the host.
 */
final class ArchiveException extends IOException {
    private static final long serialVersionUID = 1L;

    enum Kind {
        /** A member's data does not unpack to what its checksum or size says (or the archive ends inside it). */
        DAMAGED_FILE,
        /** The archive's own index (central directory, 7z header) cannot be read. */
        DAMAGED_INDEX,
        /** A compression method or feature the readers do not implement. */
        UNSUPPORTED,
        /** Password-protected data. */
        ENCRYPTED,
        /** Unpacking needs more memory than the app has. */
        MEMORY,
        /** One part of a multi-part archive. */
        SPLIT,
        /** Beyond the importer's bounds (entry count, header size, path depth). */
        LIMIT,
    }

    final Kind kind;
    /** What it concerns: a member's name, the archive's name. */
    final String subject;
    /** A method name, a size in MB: whatever the message needs besides the subject. */
    final String detail;

    ArchiveException(Kind kind, String subject, String detail) {
        super(kind + ": " + subject + (detail == null ? "" : " (" + detail + ")"));
        this.kind = kind;
        this.subject = subject;
        this.detail = detail;
    }

    ArchiveException(Kind kind, String subject) { this(kind, subject, null); }

    static ArchiveException damagedIndex(String archive, String detail) { return new ArchiveException(Kind.DAMAGED_INDEX, archive, detail); }

    /**
     * An error met while unpacking {@code member}: data errors (a bad checksum, a stream that ends early, a decoder
     * that finds garbage) become DAMAGED_FILE; other I/O errors (the storage, the provider) pass unchanged.
     */
    static IOException whileReading(IOException error, String member) {
        if (error instanceof ArchiveException) return error;
        ArchiveException mapped = null;
        if (error instanceof MemoryLimitException) mapped = new ArchiveException(Kind.MEMORY, member, String.valueOf(((MemoryLimitException) error).getMemoryNeeded() / 1024));
        else if (error instanceof UnsupportedOptionsException) mapped = new ArchiveException(Kind.UNSUPPORTED, member, error.getMessage());
        else if (error instanceof EOFException || error instanceof ZipException || error instanceof CorruptedInputException
                || error.getCause() instanceof DataFormatException)
            mapped = new ArchiveException(Kind.DAMAGED_FILE, member, error.getMessage());
        if (mapped == null) return error;
        mapped.initCause(error);
        return mapped;
    }
}
