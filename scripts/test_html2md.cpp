// Legacy converter regressions. No Qt or test framework required.
// c++ -std=c++17 -Ithird_party scripts/test_html2md.cpp third_party/html2md.cpp \
//     third_party/table.cpp -o test_html2md && ./test_html2md
#include "html2md.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
int failures = 0;

std::string convert(const std::string &html) {
  html2md::Options options;
  options.splitLines = false;
  options.includeTitle = false;
  html2md::Converter converter(html, &options);
  auto result = converter.convert();
  const auto begin = result.find_first_not_of(" \n\r\t");
  if (begin == std::string::npos)
    return {};
  return result.substr(begin, result.find_last_not_of(" \n\r\t") - begin + 1);
}

void expect(const char *label, const std::string &html, const std::string &wanted) {
  const auto actual = convert(html);
  if (actual != wanted) {
    ++failures;
    std::cerr << label << "\nexpected: " << wanted << "\nactual: " << actual << '\n';
  }
}
} // namespace

int main() {
  expect("exact attribute names", R"(<a data-href="/tracking" href="/real">Link</a>)",
         "[Link](/real)");
  expect("attribute-looking quoted text", R"(<a data-info="href='/fake'" href="/real">Link</a>)",
         "[Link](/real)");
  expect("mixed case and whitespace", "<A\tDATA-HREF='/fake'\nHrEf = '/Real' >Link</A>",
         "[Link](/Real)");
  expect("single quoted URL with greater-than", R"(<a href='/real?q=a>b'>Link</a>)",
         "[Link](/real?q=a>b)");
  expect("double quoted URL containing apostrophe", R"(<a href="/real?q=it's>fine">Link</a>)",
         "[Link](/real?q=it's>fine)");
  expect("unquoted URL before quoted attribute", R"(<a href=/real?q=1 title="Title">Link</a>)",
         "[Link](/real?q=1 \"Title\")");
  expect("unquoted slash remains part of URL", R"(<a href=/real/>Link</a>)",
         "[Link](/real/)");
  expect("absent href does not borrow another attribute", R"(<a data-href="/fake">Link</a>)",
         "[Link]()");
  expect("boolean href does not borrow following value", R"(<a href data-href="/fake">Link</a>)",
         "[Link]()");
  expect("first duplicate attribute wins", R"(<a href="/first" HREF="/second">Link</a>)",
         "[Link](/first)");
  expect("image attributes", R"(<img data-src="/fake" data-alt="wrong" SRC=/real.png ALT='Image' />)",
         "![Image](/real.png)");
  expect("nested blocks within link", R"(<a href='/real'><div><strong>Link</strong></div></a>)",
         "[**Link**](/real)");
  expect("details retain link content", R"(<details><summary>More</summary><p><a href='/real'>Link</a></p></details>)",
         "More\n[Link](/real)");
  expect("unknown aria attribute cannot hide link opener", R"(<a aria="hidden" href="/real">Link</a>)",
         "[Link](/real)");

  // Place decoy attributes AFTER the wrapped uint16_t offset. Padding alone
  // lets the old substring search rediscover the right tag, hiding the bug.
  const std::string prefix = std::string(5000, 'x') +
      R"(<a href="/old">Old</a><img src="/old.png" alt="Old image"/>)";
  for (const size_t offset : {size_t(65535), size_t(65536), size_t(70000)}) {
    const auto longResult = convert(prefix + std::string(offset - prefix.size(), 'x') +
        R"(<a href="/new">New</a><img src="/new.png" alt="New image"/>)");
    if (longResult.find("[New](/new)") == std::string::npos ||
        longResult.find("![New image](/new.png)") == std::string::npos) {
      ++failures;
      std::cerr << "links/images after 64 KiB were not extracted correctly at " << offset << '\n';
    }
  }

  if (failures)
    return EXIT_FAILURE;
  std::cout << "15 legacy html2md regressions passed\n";
  return EXIT_SUCCESS;
}
