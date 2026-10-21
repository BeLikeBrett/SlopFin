// Copyright (C) 2026 Brett
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Security.Cryptography;
using Mono.Cecil;
using Mono.Cecil.Cil;

// Only the pinned UltraPack assembly is admitted; never rewrite the cached original.
const string expected = "934E1D06D33796902CA949AC5ABF65833AF40A4A4CAA5E356847BA1CCDF7486C";
if (args.Length != 2 || Path.GetFullPath(args[0]) == Path.GetFullPath(args[1]))
    throw new ArgumentException("Usage: HomebrewPatcher <original.dll> <new-output.dll>");
if (Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(args[0]))) != expected)
    throw new InvalidDataException("Unexpected library checksum; review the new assembly first.");
using var assembly = AssemblyDefinition.ReadAssembly(args[0]);
var builder = assembly.MainModule.GetType("LibProsperoPkg.PKG.ProsperoPkgBuilder");
var method = builder.Methods.Single(m => m.Name == "EnsureAboutRightSprx");
if (!method.IsStatic || method.ReturnType.FullName != "System.Void" ||
    method.Parameters.Count != 1 || !method.HasBody)
    throw new InvalidDataException("Unexpected rights-module insertion method.");
method.Body.Instructions.Clear();
method.Body.ExceptionHandlers.Clear();
method.Body.Variables.Clear();
method.Body.GetILProcessor().Append(Instruction.Create(OpCodes.Ret));
var resource = assembly.MainModule.Resources.Single(r => r.Name.EndsWith(".right.sprx"));
assembly.MainModule.Resources.Remove(resource);
assembly.Write(args[1]);
using var checkedAssembly = AssemblyDefinition.ReadAssembly(args[1]);
var checkedMethod = checkedAssembly.MainModule.GetType(builder.FullName).Methods
    .Single(m => m.Name == method.Name);
if (checkedMethod.Body.Instructions.Count != 1 ||
    checkedMethod.Body.Instructions[0].OpCode != OpCodes.Ret ||
    checkedAssembly.MainModule.Resources.Any(r => r.Name.EndsWith(".right.sprx")))
    throw new InvalidDataException("Homebrew library patch did not survive readback.");
Console.WriteLine("Disabled automatic right.sprx insertion and removed its embedded resource.");
Console.WriteLine("Output SHA256: " + Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(args[1]))));
