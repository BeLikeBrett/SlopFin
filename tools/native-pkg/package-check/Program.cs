// Copyright (C) 2026 Brett
// SPDX-License-Identifier: GPL-3.0-or-later
// Reads every block, chunk and file of a finished package; exits 1 on any gap.
using System.Text.Json;
using LibProsperoPkg.PKG;

if (args.Length != 1)
    throw new ArgumentException("Usage: PackageCheck <package.pkg>");
var result = ProsperoPackageArchive.VerifyPackageFull(
    args[0], new string('0', 32), maxDegreeOfParallelism: 2);
Console.WriteLine(JsonSerializer.Serialize(result, new JsonSerializerOptions { WriteIndented = true }));
return result.IsValid && result.IsFull && result.OuterBlocksVerified == result.OuterBlockCount &&
    result.NapsChunksDecoded == result.NapsChunkCount && result.FilesDecoded == result.FileCount ? 0 : 1;
