mod convert;
use plamo_metal_q4::Engine;

use anyhow::{Result, bail, ensure};
use std::{path::Path, process::Command, time::Instant};

#[cfg(feature = "reference")]
mod baseline {
    use sha2::{Digest, Sha256};
    use std::{fs::File, path::Path};
    const MODEL: &str = "plamo-3-translate-0.8b-Q4_K_M.gguf";
    mod upstream {
        include!("../reference/llama.rs");
    }
    pub use upstream::Translator;
}

fn main() -> Result<()> {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    if let [cmd, dir, out] = args.as_slice()
        && cmd == "convert"
    {
        return convert::convert(dir.as_ref(), out.as_ref());
    }
    let [method, model, pdf, page, runs] = args.as_slice() else {
        bail!(
            "usage: plamo-metal-q4 convert BF16_DIRECTORY OUTPUT | q4|reference MODEL PDF PAGE RUNS | compare-head Q4_MODEL PDF PAGE GGUF_MODEL"
        );
    };
    let page: usize = page.to_str().unwrap_or("").parse()?;
    #[cfg(feature = "reference")]
    let reference_path = (method == "compare-head").then(|| Path::new(runs));
    let runs: usize = if method == "compare-head" {
        1
    } else {
        runs.to_str().unwrap_or("").parse()?
    };
    ensure!(page > 0 && runs > 0);
    // Development benchmark. Both engines receive exactly the same PDF extraction.
    let page = page.to_string();
    let pdf = std::fs::canonicalize(pdf)?;
    let extraction = Command::new("pdftotext")
        .args(["-enc", "UTF-8", "-f", &page, "-l", &page, "-nopgbrk"])
        .arg(pdf)
        .arg("-")
        .output()?;
    ensure!(
        extraction.status.success(),
        "{}",
        String::from_utf8_lossy(&extraction.stderr)
    );
    let page_text = std::str::from_utf8(&extraction.stdout)?.trim();
    ensure!(!page_text.is_empty());
    let opened = Instant::now();
    #[cfg(feature = "reference")]
    if let Some(reference_path) = reference_path {
        let e = Engine::open(Path::new(model))?;
        let mut reference =
            baseline::Translator::open(reference_path).map_err(anyhow::Error::msg)?;
        let mut states = 0usize;
        let mut matched = 0usize;
        let mut elapsed = Vec::new();
        let mut sum_error = 0.0f64;
        let mut sum_reference = 0.0f64;
        let best = |scores: &[f32]| {
            scores
                .iter()
                .enumerate()
                .max_by(|a, b| a.1.total_cmp(b.1).then(b.0.cmp(&a.0)))
                .unwrap()
                .0
        };
        reference
            .translate(
                page_text,
                || false,
                Some(&mut |hidden, q6| {
                    let start = Instant::now();
                    let q4 = e.score_head(hidden).map_err(|e| e.to_string())?;
                    elapsed.push(start.elapsed());
                    matched += usize::from(best(q6) == best(&q4));
                    states += 1;
                    for (&a, &b) in q6.iter().zip(&q4) {
                        sum_error += f64::from(a - b).powi(2);
                        sum_reference += f64::from(a).powi(2);
                    }
                    Ok(())
                }),
            )
            .map_err(anyhow::Error::msg)?;
        elapsed.sort();
        println!(
            "states={states} top1_matches={matched} relative_rms={:.6} q4_head_ms={:.4}",
            (sum_error / sum_reference).sqrt(),
            elapsed[elapsed.len() / 2].as_secs_f64() * 1e3
        );
        return Ok(());
    }
    if method == "q4" {
        let e = Engine::open(Path::new(model))?;
        eprintln!("model_open_s={:.4}", opened.elapsed().as_secs_f64());
        for run in 0..runs {
            let v = e.translate(page_text, || false)?;
            println!(
                "run={} input={} output={} first_scores_s={:.4} total_s={:.4}",
                run,
                v.input,
                v.output,
                v.prefill.as_secs_f64(),
                v.total.as_secs_f64()
            );
            if run == 0 {
                println!("{}", v.text);
            }
        }
        return Ok(());
    }
    #[cfg(feature = "reference")]
    if method == "reference" {
        let mut e = baseline::Translator::open(Path::new(model)).map_err(anyhow::Error::msg)?;
        eprintln!("model_open_s={:.4}", opened.elapsed().as_secs_f64());
        for run in 0..runs {
            let v = e
                .translate(page_text, || false, None)
                .map_err(anyhow::Error::msg)?;
            println!(
                "run={} input={} output={} first_scores_s={:.4} total_s={:.4}",
                run,
                v.input,
                v.output,
                v.prefill.as_secs_f64(),
                v.total.as_secs_f64()
            );
            if run == 0 {
                println!("{}", v.text);
            }
        }
        return Ok(());
    }
    bail!("unknown method or reference feature disabled")
}
