use font_kit::{
    family_name::FamilyName, handle::Handle, properties::Properties, source::SystemSource,
};
use hayro::hayro_interpret::{InterpreterSettings, font::FontQuery, hayro_cmap::CidFamily};
use std::{
    collections::HashMap,
    sync::{Arc, Mutex},
};

/// Ask the native font collection for substitutes only when the PDF has
/// omitted a font. Keep font data bounded to this open document.
pub fn settings() -> InterpreterSettings {
    let mut settings = InterpreterSettings::default();
    let standard = settings.font_resolver.clone();
    let cache = Mutex::new(HashMap::new());
    settings.font_resolver = Arc::new(move |query| {
        let FontQuery::Fallback(f) = query else {
            return standard(query);
        };
        let family = f.character_collection.as_ref().map(|c| &c.family);
        #[cfg(windows)]
        let fallback = match family {
            Some(CidFamily::AdobeJapan1) => "Yu Gothic",
            Some(CidFamily::AdobeKorea1) => "Malgun Gothic",
            Some(CidFamily::AdobeGB1) => "Microsoft YaHei",
            Some(CidFamily::AdobeCNS1) => "Microsoft JhengHei",
            _ => "",
        };
        #[cfg(target_os = "macos")]
        let fallback = match family {
            Some(CidFamily::AdobeJapan1) => "Hiragino Sans",
            Some(CidFamily::AdobeKorea1) => "Apple SD Gothic Neo",
            Some(CidFamily::AdobeGB1) => "PingFang SC",
            Some(CidFamily::AdobeCNS1) => "PingFang TC",
            _ => "",
        };
        for name in [
            f.post_script_name.as_deref(),
            f.font_family.as_deref(),
            Some(fallback),
        ]
        .into_iter()
        .flatten()
        .filter(|s| !s.is_empty())
        {
            let mut cache = cache.lock().unwrap();
            if let Some(hit) = cache.get(name) {
                return Some(Clone::clone(hit));
            }
            let system = SystemSource::new();
            let handle = system.select_by_postscript_name(name).or_else(|_| {
                system.select_best_match(&[FamilyName::Title(name.into())], &Properties::new())
            });
            let (bytes, index) = match handle {
                Ok(Handle::Memory { bytes, font_index }) => (bytes, font_index),
                Ok(Handle::Path { path, font_index }) => match std::fs::read(path) {
                    Ok(data) => (Arc::new(data), font_index),
                    Err(_) => continue,
                },
                Err(_) => continue,
            };
            let data: Arc<dyn AsRef<[u8]> + Send + Sync> = bytes;
            if cache.len() < 4 {
                cache.insert(name.to_owned(), (data.clone(), index));
            }
            return Some((data, index));
        }
        standard(query)
    });
    settings
}
