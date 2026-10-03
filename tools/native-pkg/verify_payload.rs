// Copyright (C) 2026 Brett
// SPDX-License-Identifier: GPL-3.0-or-later
use ps5upload_fpkg::{
    PkgFile, cnt, cnt_write, crypto, fih, inner, kraken_image, outer, sdk_rules, self_repair,
    source as package_source,
};
use std::{
    collections::{BTreeMap, BTreeSet},
    error::Error,
    fs,
    path::Path,
};

fn verify(package: &Path, source: &Path) -> Result<(), Box<dyn Error>> {
    let mut pkg = PkgFile::open(package)?;
    let header = pkg.read_at(0, fih::HEADER_LEN)?;
    let envelope = fih::parse(&header)?;
    let container = cnt::read(&mut pkg, envelope.cnt_offset)?;
    let image = outer::open(&mut pkg, &envelope, &container, crypto::DEFAULT_PASSCODE)?;
    let nodes = image.dinodes();
    let root = image.dirents(nodes.get(2).ok_or("Missing outer root")?);
    let read = |name: &str| -> Result<Vec<u8>, Box<dyn Error>> {
        let entry = root
            .iter()
            .find(|d| d.name == name)
            .ok_or("Missing image entry")?;
        Ok(image.file_data(nodes.get(entry.ino as usize).ok_or("Invalid image inode")?))
    };
    let compressed = read("pfs_image.dat")?;
    let blocks = kraken_image::describe(&read("naps_pkg_layout.dat")?)?;
    let length = blocks
        .iter()
        .map(|b| b.logical + b.len)
        .max()
        .ok_or("Empty descriptor")?;
    if length > 256 * 1024 * 1024 {
        return Err("Unexpectedly large SlopFin mount".into());
    }
    let mut mount = vec![0; length as usize];
    for block in &blocks {
        let decoded = kraken_image::decode_described(&compressed, block)?;
        if decoded.len() as u64 != block.len {
            return Err("Decoded block length mismatch".into());
        }
        mount[block.logical as usize..(block.logical + block.len) as usize]
            .copy_from_slice(&decoded);
    }
    let meta_base = u32::from_le_bytes(header[0x50..0x54].try_into()?) as u64 * 65536;
    let walked = inner::read(&mount, meta_base)?;
    if !walked.flt_ok {
        return Err("Inner flat-path table mismatch".into());
    }
    let mut expected = BTreeSet::new();
    inventory(source, source, &mut expected)?;
    let mut expected_bytes = BTreeMap::new();
    for name in &expected {
        let original = fs::read(source.join(name))?;
        let mut read_original =
            |at: u64, length: usize| Ok(original[at as usize..at as usize + length].to_vec());
        let bytes = match self_repair::plan(&original, original.len() as u64, &mut read_original) {
            Some(repair) => repair.read(
                original.len() as u64,
                0,
                repair.new_size(original.len() as u64) as usize,
                &mut read_original,
            )?,
            None => original,
        };
        expected_bytes.insert(name.clone(), bytes);
    }
    let unpacked = expected_bytes
        .iter()
        .filter(|(name, _)| name.as_str() != "sce_sys/param.json")
        .map(|(_, b)| b.len() as u64)
        .sum();
    let param = expected_bytes
        .get_mut("sce_sys/param.json")
        .ok_or("Missing source metadata")?;
    *param = package_source::drm_rewrite(param).unwrap_or_else(|| param.clone());
    *param = package_source::launch_rewrite(param).unwrap_or_else(|| param.clone());
    if let Some((rewritten, _)) =
        sdk_rules::size_class_rewrite(param, unpacked, expected.len() as u64)?
    {
        *param = rewritten;
    }
    let mut checked = BTreeSet::new();
    for file in &walked.files {
        let name = file.path.as_str();
        if Path::new(name)
            .components()
            .any(|c| !matches!(c, std::path::Component::Normal(_)))
        {
            return Err("Unsafe mount path".into());
        }
        if name == "sce_sys/keystone" {
            continue;
        }
        let expected = expected_bytes
            .get(name)
            .ok_or_else(|| format!("Unexpected packaged file: {name}"))?;
        let actual = mount
            .get(file.offset as usize..(file.offset + file.size) as usize)
            .ok_or("File outside mount")?;
        if expected.as_slice() != actual {
            return Err(format!("Payload mismatch: {name}").into());
        }
        checked.insert(name.to_string());
    }
    let presentations = [
        (cnt::ids::ICON0_PNG, "sce_sys/icon0.png"),
        (cnt::ids::ICON0_DDS, "sce_sys/icon0.dds"),
    ];
    for (id, name) in presentations.into_iter().chain(
        cnt_write::PRESENTATION
            .iter()
            .map(|&(id, path, _)| (id, path)),
    ) {
        if !source.join(name).is_file() || checked.contains(name) {
            continue;
        }
        let entry = container
            .entries
            .iter()
            .find(|e| e.id == id)
            .ok_or("Missing presentation entry")?;
        let actual = container
            .bytes
            .get(entry.offset as usize..(entry.offset + entry.size) as usize)
            .ok_or("Presentation outside container")?;
        if fs::read(source.join(name))? != actual {
            return Err(format!("Presentation mismatch: {name}").into());
        }
        checked.insert(name.to_string());
    }
    fn inventory(path: &Path, root: &Path, files: &mut BTreeSet<String>) -> std::io::Result<()> {
        for entry in fs::read_dir(path)? {
            let entry = entry?;
            if entry.file_type()?.is_dir() {
                inventory(&entry.path(), root, files)?;
            } else {
                files.insert(
                    entry
                        .path()
                        .strip_prefix(root)
                        .unwrap()
                        .to_string_lossy()
                        .replace('\\', "/"),
                );
            }
        }
        Ok(())
    }
    if checked != expected {
        return Err(format!(
            "File inventory mismatch: missing {:?}, extra {:?}",
            expected.difference(&checked),
            checked.difference(&expected)
        )
        .into());
    }
    println!(
        "Payload verified: {} decoded blocks, {} source files, valid inner paths; console launch remains a separate test.",
        blocks.len(),
        checked.len()
    );
    Ok(())
}

fn main() -> Result<(), Box<dyn Error>> {
    let arguments: Vec<_> = std::env::args_os().skip(1).collect();
    if arguments.len() != 2 {
        return Err("Usage: slopfin-package-check <package> <source-folder>".into());
    }
    verify(Path::new(&arguments[0]), Path::new(&arguments[1]))
}
