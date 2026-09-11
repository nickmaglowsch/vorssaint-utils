// WP-04: does the app still behave correctly after libicudata is trimmed?
//
// "The app starts" is not evidence: Qt only touches ICU when something asks
// for locale-aware collation, a non-default locale's formats, or a legacy
// text codec. This probe exercises exactly those three, for the thirteen
// languages the product ships, so the trimmed and untrimmed bundles can be
// diffed byte for byte.
//
//   g++ -std=c++17 icu-probe.cpp $(pkg-config --cflags --libs Qt6Core) -o icu-probe
#include <QCollator>
#include <QDate>
#include <QLocale>
#include <QStringConverter>
#include <QTextStream>
#include <QStringList>

int main()
{
    QTextStream out(stdout);
    const QStringList langs = { "en_US", "de_DE", "es_ES", "fr_FR", "it_IT",
                                "ja_JP", "ko_KR", "pt_BR", "ru_RU", "tr_TR",
                                "zh_Hans_CN", "zh_Hant_HK", "zh_Hant_TW",
                                // not in the trimmed set, on purpose:
                                "pl_PL", "th_TH" };
    const QDate d(2026, 3, 9);
    for (const QString &tag : langs) {
        QLocale l(tag);
        out << "locale " << tag
            << " | name=" << l.name()
            << " | date=" << l.toString(d, QLocale::LongFormat)
            << " | month=" << l.monthName(3)
            << " | num=" << l.toString(1234567.89)
            << " | cur=" << l.toCurrencyString(12.5) << "\n";

        QCollator c(l);
        QStringList s = { QString::fromUtf8("zebra"), QString::fromUtf8("Äpfel"),
                          QString::fromUtf8("apple"), QString::fromUtf8("Ø"),
                          QString::fromUtf8("ćma"), QString::fromUtf8("czas") };
        std::sort(s.begin(), s.end(), [&](const QString &a, const QString &b) {
            return c.compare(a, b) < 0;
        });
        out << "  collate " << tag << " -> " << s.join(',') << "\n";
    }
    // A legacy converter, the kind icupkg's -r on *.cnv would remove.
    for (const char *codec : { "Shift_JIS", "windows-1251", "ISO 8859-7", "Big5" }) {
        auto enc = QStringConverter::encodingForName(codec);
        out << "codec " << codec << " -> "
            << (enc ? "available" : "MISSING") << "\n";
    }
    return 0;
}
