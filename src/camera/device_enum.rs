#[derive(Debug, Clone)]
pub struct CameraDevice {
    pub name: String,
    pub manufacturer: Option<String>,
    pub device_path: Option<String>,
    pub driver: Option<String>,
    pub vid: Option<String>,
    pub pid: Option<String>,
    pub clsid: Option<String>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DetectionResult {
    RealCamera,
    VirtualCamera,
    NoCamera,
}

pub fn enumerate_devices() -> Vec<CameraDevice> {
    #[cfg(windows)]
    {
        enumerate_windows_devices()
    }

    #[cfg(not(windows))]
    {
        Vec::new()
    }
}

pub fn detect_cameras() -> DetectionResult {
    let devices = enumerate_devices();

    if devices.is_empty() {
        return DetectionResult::NoCamera;
    }

    let mut has_real = false;
    let mut has_virtual = false;

    for device in &devices {
        if is_virtual_camera(device) {
            has_virtual = true;
        } else {
            has_real = true;
        }
    }

    if has_real {
        DetectionResult::RealCamera
    } else if has_virtual {
        DetectionResult::VirtualCamera
    } else {
        DetectionResult::NoCamera
    }
}

fn is_virtual_camera(device: &CameraDevice) -> bool {
    let mut haystack = String::new();
    haystack.push_str(&device.name.to_lowercase());
    if let Some(value) = &device.manufacturer {
        haystack.push_str(&value.to_lowercase());
    }
    if let Some(value) = &device.driver {
        haystack.push_str(&value.to_lowercase());
    }
    if let Some(value) = &device.device_path {
        haystack.push_str(&value.to_lowercase());
    }

    let name_blacklist = [
        "virtual",
        "obs",
        "manycam",
        "snap camera",
        "xsplit",
        "mmhmm",
        "droidcam",
        "iriun",
        "contacam",
        "streamlabs",
        "camsip",
    ];

    if name_blacklist.iter().any(|needle| haystack.contains(needle)) {
        return true;
    }

    let clsid_blacklist = [
        "{860bb310-5d01-11d0-bd3b-00a0c911ce86}", // CLSID_VideoInputDeviceCategory
        "{e5323777-f976-4f5b-9b55-b94699c46e44}", // CLSID_SampleGrabber (often virtual filters)
    ];

    if let Some(clsid) = &device.clsid {
        let clsid_lower = clsid.to_lowercase();
        if clsid_blacklist.iter().any(|needle| clsid_lower.contains(needle)) {
            return true;
        }
    }

    let vid_pid_blacklist = [
        ("0bda", "58f4"), // OBS Virtual Camera
        ("0c45", "6366"), // ManyCam Virtual Webcam
        ("2b7e", "f13a"), // Snap Camera
        ("05a3", "9331"), // DroidCam
    ];

    if let (Some(vid), Some(pid)) = (&device.vid, &device.pid) {
        let vid_lower = vid.to_lowercase();
        let pid_lower = pid.to_lowercase();
        if vid_pid_blacklist
            .iter()
            .any(|(v, p)| *v == vid_lower && *p == pid_lower)
        {
            return true;
        }
    }

    false
}

#[cfg(windows)]
fn enumerate_windows_devices() -> Vec<CameraDevice> {
    let mut devices = enumerate_media_foundation_devices();
    let mut directshow_devices = enumerate_directshow_devices();
    devices.append(&mut directshow_devices);
    devices
}

#[cfg(windows)]
fn enumerate_media_foundation_devices() -> Vec<CameraDevice> {
    use windows::Win32::Media::MediaFoundation::{
        MFCreateAttributes, MFEnumDeviceSources, MFShutdown, MFStartup,
        MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, MF_VERSION,
    };
    use windows::Win32::System::Com::{CoInitializeEx, CoUninitialize, COINIT_MULTITHREADED};

    let mut devices = Vec::new();

    unsafe {
        if CoInitializeEx(None, COINIT_MULTITHREADED).is_err() {
            return devices;
        }
        if MFStartup(MF_VERSION, 0).is_err() {
            CoUninitialize();
            return devices;
        }

        let mut attributes = None;
        if MFCreateAttributes(&mut attributes, 1).is_err() {
            MFShutdown().ok();
            CoUninitialize();
            return devices;
        }
        let attributes = attributes.unwrap();
        if attributes
            .SetGUID(
                &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID,
            )
            .is_err()
        {
            MFShutdown().ok();
            CoUninitialize();
            return devices;
        }

        let mut activates = None;
        let mut count = 0;
        if MFEnumDeviceSources(&attributes, &mut activates, &mut count).is_ok() {
            if let Some(activates) = activates {
                for index in 0..count {
                    if let Some(activate) = activates.get(index as usize) {
                        let name = get_activate_string(&activate, &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME)
                            .unwrap_or_else(|| "Unknown Camera".to_string());
                        let device_path = get_activate_string(
                            &activate,
                            &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                        );
                        let (vid, pid) = parse_vid_pid(device_path.as_deref());

                        let device = CameraDevice {
                            name,
                            manufacturer: None,
                            device_path,
                            driver: None,
                            vid,
                            pid,
                            clsid: None,
                        };
                        devices.push(device);
                    }
                }
            }
        }

        MFShutdown().ok();
        CoUninitialize();
    }

    devices
}

#[cfg(windows)]
fn enumerate_directshow_devices() -> Vec<CameraDevice> {
    use windows::core::Interface;
    use windows::Win32::Media::DirectShow::{
        CLSID_SystemDeviceEnum, CLSID_VideoInputDeviceCategory, ICreateDevEnum,
        IEnumMoniker, IMoniker,
    };
    use windows::Win32::System::Com::{
        CoCreateInstance, CoInitializeEx, CoUninitialize, CLSCTX_INPROC_SERVER,
        COINIT_MULTITHREADED,
    };

    let mut devices = Vec::new();

    unsafe {
        if CoInitializeEx(None, COINIT_MULTITHREADED).is_err() {
            return devices;
        }

        let enumerator: ICreateDevEnum = match CoCreateInstance(
            &CLSID_SystemDeviceEnum,
            None,
            CLSCTX_INPROC_SERVER,
        ) {
            Ok(enumerator) => enumerator,
            Err(_) => {
                CoUninitialize();
                return devices;
            }
        };

        let mut class_enum: Option<IEnumMoniker> = None;
        if enumerator
            .CreateClassEnumerator(&CLSID_VideoInputDeviceCategory, &mut class_enum, 0)
            .is_err()
        {
            CoUninitialize();
            return devices;
        }

        let mut class_enum = match class_enum {
            Some(class_enum) => class_enum,
            None => {
                CoUninitialize();
                return devices;
            }
        };

        loop {
            let mut monikers: [Option<IMoniker>; 1] = [None];
            let mut fetched = 0;
            if class_enum.Next(&mut monikers, &mut fetched).is_err() || fetched == 0 {
                break;
            }

            let Some(moniker) = monikers[0].take() else { continue };

            let mut property_bag = None;
            if moniker.BindToStorage(None, None, &mut property_bag).is_err() {
                continue;
            }

            let Some(property_bag) = property_bag else { continue };

            let name = read_property_bag_string(&property_bag, "FriendlyName")
                .unwrap_or_else(|| "Unknown Camera".to_string());
            let manufacturer = read_property_bag_string(&property_bag, "Manufacturer");
            let device_path = read_property_bag_string(&property_bag, "DevicePath");
            let driver = read_property_bag_string(&property_bag, "Driver");
            let clsid = read_property_bag_string(&property_bag, "CLSID");
            let (vid, pid) = parse_vid_pid(device_path.as_deref());

            devices.push(CameraDevice {
                name,
                manufacturer,
                device_path,
                driver,
                vid,
                pid,
                clsid,
            });
        }

        CoUninitialize();
    }

    devices
}

#[cfg(windows)]
fn get_activate_string(
    activate: &windows::Win32::Media::MediaFoundation::IMFActivate,
    key: &windows::core::GUID,
) -> Option<String> {
    use windows::core::PWSTR;

    unsafe {
        let mut string_ptr = PWSTR::null();
        let mut length = 0;
        if activate
            .GetAllocatedString(key, &mut string_ptr, &mut length)
            .is_err()
        {
            return None;
        }
        if string_ptr.is_null() {
            return None;
        }
        let slice = std::slice::from_raw_parts(string_ptr.0, length as usize);
        let string = String::from_utf16_lossy(slice);
        windows::Win32::System::Com::CoTaskMemFree(Some(string_ptr.0 as _));
        Some(string)
    }
}

#[cfg(windows)]
fn read_property_bag_string(
    property_bag: &windows::Win32::Media::DirectShow::IPropertyBag,
    name: &str,
) -> Option<String> {
    use windows::core::BSTR;
    use windows::Win32::System::Com::VARIANT;

    unsafe {
        let mut variant = VARIANT::default();
        if property_bag
            .Read(&BSTR::from(name), &mut variant, None)
            .is_err()
        {
            return None;
        }

        if variant.Anonymous.Anonymous.vt as u32
            != windows::Win32::System::Variant::VT_BSTR.0
        {
            return None;
        }

        let bstr = variant.Anonymous.Anonymous.Anonymous.bstrVal;
        if bstr.is_null() {
            return None;
        }
        Some(BSTR::from_raw(bstr).to_string())
    }
}

fn parse_vid_pid(device_path: Option<&str>) -> (Option<String>, Option<String>) {
    let Some(device_path) = device_path else {
        return (None, None);
    };

    let device_path_lower = device_path.to_lowercase();
    let vid = extract_segment(&device_path_lower, "vid_");
    let pid = extract_segment(&device_path_lower, "pid_");
    (vid, pid)
}

fn extract_segment(source: &str, token: &str) -> Option<String> {
    let start = source.find(token)? + token.len();
    let segment = source[start..].chars().take(4).collect::<String>();
    if segment.len() == 4 {
        Some(segment)
    } else {
        None
    }
}
