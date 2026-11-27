use crate::document::PageImage;

#[cfg(windows)]
pub fn recognize(image: &PageImage) -> Result<String, String> {
    use windows::{
        Graphics::Imaging::{BitmapPixelFormat, SoftwareBitmap},
        Media::Ocr::OcrEngine,
        Security::Cryptography::CryptographicBuffer,
        Win32::System::WinRT::{RO_INIT_MULTITHREADED, RoInitialize, RoUninitialize},
    };
    fn run(image: &PageImage) -> windows::core::Result<String> {
        let buf = CryptographicBuffer::CreateFromByteArray(&image.rgba)?;
        let bitmap = SoftwareBitmap::CreateCopyFromBuffer(
            &buf,
            BitmapPixelFormat::Rgba8,
            image.width as i32,
            image.height as i32,
        )?;
        let engine = OcrEngine::TryCreateFromUserProfileLanguages()?;
        let result = engine.RecognizeAsync(&bitmap)?.join()?;
        let mut lines = Vec::new();
        for line in result.Lines()? {
            lines.push(line.Text()?.to_string());
        }
        Ok(lines.join("\n"))
    }
    unsafe { RoInitialize(RO_INIT_MULTITHREADED) }.map_err(|e| e.to_string())?;
    struct Apartment;
    impl Drop for Apartment {
        fn drop(&mut self) {
            unsafe { RoUninitialize() };
        }
    }
    let _apartment = Apartment;
    run(image).map_err(|_| "このページの画像から文字を読み取れませんでした。".into())
}

#[cfg(target_os = "macos")]
pub fn recognize(image: &PageImage) -> Result<String, String> {
    use objc2::{AnyThread, rc::autoreleasepool};
    use objc2_foundation::{NSArray, NSData, NSDictionary};
    use objc2_vision::{VNImageRequestHandler, VNRecognizeTextRequest, VNRequest};
    autoreleasepool(|_| {
        let request = unsafe { VNRecognizeTextRequest::init(VNRecognizeTextRequest::alloc()) };
        request.setAutomaticallyDetectsLanguage(true);
        let data = NSData::with_bytes(&image.png);
        let handler = VNImageRequestHandler::initWithData_options(
            VNImageRequestHandler::alloc(),
            &data,
            &NSDictionary::new(),
        );
        let requests = NSArray::from_slice(&[&request as &VNRequest]);
        handler
            .performRequests_error(&requests)
            .map_err(|e| e.to_string())?;
        let mut lines = Vec::new();
        if let Some(results) = request.results() {
            for observation in results.iter() {
                if let Some(top) = observation.topCandidates(1).firstObject() {
                    lines.push(top.string().to_string());
                }
            }
        }
        Ok(lines.join("\n"))
    })
}
