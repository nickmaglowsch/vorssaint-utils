#!/usr/bin/env python3
import re, os, json
from collections import Counter

import pathlib
ROOT = str(pathlib.Path(__file__).resolve().parents[2])
files = [l.strip() for l in open(os.environ.get("WP00_FILES", os.path.join(os.path.dirname(os.path.abspath(__file__)), "files-all.txt"))) if l.strip()]

# Present in swift-corelibs-foundation on Linux
NS_OK = set("""NSObject NSString NSNumber NSRange NSError NSLock NSRegularExpression
NSNotFound NSTextCheckingResult NSMakeRange NSNotification NSNull NSCharacterSet NSData NSDate
NSArray NSDictionary NSSet NSURL NSMutableString NSMutableArray NSMutableDictionary NSExpression
NSPredicate NSLocale NSCalendar NSTimeZone NSDecimalNumber NSDecimal NSKeyedArchiver
NSKeyedUnarchiver NSCoder NSSecureCoding NSCoding NSCopying NSUUID NSSortDescriptor NSValue
NSOperationQueue NSOperation NSRecursiveLock NSCondition NSItemProvider NSHomeDirectory
NSTemporaryDirectory NSSearchPathForDirectoriesInDomains NSUserName NSFullUserName
NSCocoaErrorDomain NSPOSIXErrorDomain NSOSStatusErrorDomain NSUnderlyingErrorKey
NSLocalizedDescriptionKey NSLocalizedFailureReasonErrorKey NSLocalizedRecoverySuggestionErrorKey
NSFilePathErrorKey NSURLErrorKey NSFileReadNoPermissionError NSFileReadNoSuchFileError
NSFileWriteNoPermissionError NSFileWriteFileExistsError NSFileNoSuchFileError
NSRect NSSize NSPoint NSEdgeInsets NSZeroRect NSZeroSize NSZeroPoint NSLog
NSJSONSerialization NSCache NSCountedSet NSOrderedSet NSIndexSet NSIndexPath
NSPersonNameComponents NSMeasurement NSFormatter NSNumberFormatter NSDateFormatter
NSByteCountFormatter NSDateComponentsFormatter NSStringEncoding NSStringCompareOptions
NSDataDetector NSPropertyListSerialization NSFileManager NSProcessInfo NSThread NSTimer
NSRunLoop NSStream NSInputStream NSOutputStream NSPipe NSTask NSBundle NSUserDefaults
NSMutableData NSMutableSet NSMutableOrderedSet NSMutableIndexSet NSMutableCharacterSet
NSURLComponents NSURLQueryItem NSURLRequest NSURLSession NSHTTPURLResponse NSURLResponse
NSCalendarUnit NSComparisonResult NSEnumerationOptions NSDirectionalEdgeInsets""".split())
CG_OK = {"CGFloat","CGPoint","CGSize","CGRect","CGVector","CGAffineTransform"}

BAD_IMPORTS = set("""AppKit SwiftUI Cocoa Carbon Carbon.HIToolbox IOKit CoreGraphics QuartzCore
CoreVideo AVFoundation CoreMedia ScreenCaptureKit Vision UniformTypeIdentifiers ServiceManagement
UserNotifications Darwin ApplicationServices CoreAudio AudioToolbox MediaPlayer CoreImage Metal
MetalKit CoreText CoreBluetooth IOBluetooth SystemConfiguration Security LocalAuthentication
OSLog os CoreLocation EventKit Contacts Photos Speech NaturalLanguage Accelerate simd CoreML
WebKit PDFKit QuickLook QuickLookThumbnailing VideoToolbox CoreServices DiskArbitration
HIDEventSystem VMStatisticsCompat CryptoKit ImageIO MachO ObjectiveC""".split())
# CoreGraphics is a special case: only the CGFloat/CGPoint/CGSize/CGRect family maps to Foundation
SOFT_IMPORTS = {"CoreGraphics"}
BAD_IMPORTS -= SOFT_IMPORTS

def strip(src):
    s = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    s = re.sub(r"//[^\n]*", " ", s)
    s = re.sub(r'"""(?:.|\n)*?"""', ' "" ', s)
    s = re.sub(r'"(?:\\.|[^"\\\n])*"', ' "" ', s)
    return s

rows = []
for rel in files:
    src = open(os.path.join(ROOT, rel), encoding="utf-8").read()
    lines = src.count("\n") + (0 if src.endswith("\n") else 1)
    code = strip(src)
    imports = sorted(set(re.findall(
        r"^\s*(?:@[A-Za-z_]+\s+)?import\s+([A-Za-z_][A-Za-z0-9_.]*)", code, re.M)))
    has_swiftui = "SwiftUI" in imports
    f = {}
    def add(k, items):
        items = sorted(set(items))
        if items: f[k] = items

    add("NS", [m for m in re.findall(r"\bNS[A-Z]\w+", code) if m not in NS_OK])
    add("CG", [m for m in re.findall(r"\bCG[A-Z]\w+", code) if m not in CG_OK])
    ui = []
    if has_swiftui:
        for sym in ["Color","Image","Font","View","Text","EnvironmentObject","StateObject",
                    "ViewBuilder","Binding","LinearGradient","Angle"]:
            if re.search(r"\b%s\b" % sym, code): ui.append("SwiftUI."+sym)
    add("UI", ui)

    comb = [s for s in ["@Published","ObservableObject","AnyCancellable","PassthroughSubject",
                        "CurrentValueSubject","ObservableObjectPublisher","objectWillChange",
                        "@ObservedObject","@StateObject"] if s in code]
    if "Combine" in imports: comb.append("import Combine")
    add("Combine", comb)

    fnd = []
    if re.search(r"\bUserDefaults\b", code): fnd.append("UserDefaults*")
    if re.search(r"Bundle\.main", code): fnd.append("Bundle.main!")
    elif re.search(r"\bBundle\b", code): fnd.append("Bundle")
    if re.search(r"\bProcessInfo\b", code): fnd.append("ProcessInfo*")
    if re.search(r"\btrashItem\b", code): fnd.append("trashItem!!")
    if re.search(r"\bFileManager\b", code): fnd.append("FileManager*")
    if re.search(r"\bNSAttributedString\b", code): fnd.append("NSAttributedString?")
    add("Foundation", fnd)

    plat = []
    for pat, name in [(r"\bos_log\b","os_log"), (r"\bLogger\b","Logger"),
                      (r"#available\(macOS","#available(macOS)"),
                      (r"@available\(macOS","@available(macOS)"),
                      (r"\bsysctl\w*","sysctl"), (r"\bmach_\w+","mach_*"),
                      (r"#if\s+os\(macOS\)","#if os(macOS)"),
                      (r"\bDispatchQueue\b","DispatchQueue*"),
                      (r"@MainActor","@MainActor*")]:
        if re.search(pat, code): plat.append(name)
    add("Platform", plat)

    bad = [i for i in imports if i in BAD_IMPORTS]
    soft = [i for i in imports if i in SOFT_IMPORTS]
    hard_syms = f.get("NS", []) + f.get("CG", []) + f.get("UI", []) + \
                [x for x in f.get("Platform", []) if x in ("os_log","Logger","sysctl","mach_*")] + \
                [x for x in f.get("Foundation", []) if x.endswith("!!")]
    if bad:
        cls = "not portable"
    elif hard_syms:
        cls = "needs small extraction" if len(hard_syms) <= 3 else "not portable"
    elif soft:
        cls = "clean (CoreGraphics types only)"
    elif f.get("Combine"):
        cls = "needs OpenCombine only"
    else:
        cls = "clean"
    rows.append(dict(file=rel, lines=lines, imports=imports, bad=bad, soft=soft,
                     findings=f, hard=hard_syms, cls=cls))

json.dump(rows, open("census.json","w"), indent=1)
print(Counter(r["cls"] for r in rows))
print("files", len(rows), "lines", sum(r["lines"] for r in rows))
print(Counter(i for r in rows for i in r["imports"]))
