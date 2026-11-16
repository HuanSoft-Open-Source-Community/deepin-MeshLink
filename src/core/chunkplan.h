// deepin-MeshLink — chunk planning math (pure, unit-testable; UC-10/11/12).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include <QtGlobal>
#include <vector>

namespace meshlink {

// Spec ranges: recommended chunk 64 KiB–1 MiB (UC-10 step 5 / BR-21).
inline constexpr quint64 kChunkSizeMin = 64ull * 1024;
inline constexpr quint64 kChunkSizeMax = 1ull * 1024 * 1024;

inline quint64 clampChunkSize(quint64 want) {
    if (want < kChunkSizeMin)
        return kChunkSizeMin;
    if (want > kChunkSizeMax)
        return kChunkSizeMax;
    return want;
}

// Number of chunks for a file of `fileSize` bytes (0-size file = 1 chunk of 0 bytes is
// normalised to 0 chunks: empty files are carried inline via blobs, not tasks).
inline quint64 totalChunks(qint64 fileSize, quint64 chunkSize) {
    if (fileSize <= 0 || chunkSize == 0)
        return 0;
    return (quint64(fileSize) + chunkSize - 1) / chunkSize;
}

inline qint64 chunkOffset(quint64 index, quint64 chunkSize) {
    return qint64(index * chunkSize);
}

inline quint64 chunkLength(quint64 index, qint64 fileSize, quint64 chunkSize) {
    const qint64 off = chunkOffset(index, chunkSize);
    if (off >= fileSize)
        return 0;
    return quint64(qMin(fileSize - off, qint64(chunkSize)));
}

// Bisection strategy for UC-12 E2: given whole-file hash mismatch and no per-chunk
// hashes, produce the candidate corrupt index list by earliest-first segments.
// MVP deterministic implementation: scan order = oldest chunks first.
inline std::vector<quint64> rescanOrderNoChunkHash(quint64 count) {
    std::vector<quint64> order;
    order.reserve(size_t(count));
    for (quint64 i = 0; i < count; ++i)
        order.push_back(i);
    return order;
}

} // namespace meshlink
