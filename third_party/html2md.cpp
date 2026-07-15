// Copyright (c) Tim Gromeyer
// Licensed under the MIT License - https://opensource.org/licenses/MIT

#include "html2md.h"
#include "table.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <sstream>
#include <vector>

using std::make_shared;
using std::string;
using std::vector;

namespace {
bool startsWith(const string &str, const string &prefix) {
  return str.size() >= prefix.size() &&
         0 == str.compare(0, prefix.size(), prefix);
}

bool endsWith(const string &str, const string &suffix) {
  return str.size() >= suffix.size() &&
         0 == str.compare(str.size() - suffix.size(), suffix.size(), suffix);
}

size_t ReplaceAll(string *haystack, const string &needle,
                  const string &replacement) {
  // Get first occurrence
  size_t pos = (*haystack).find(needle);

  size_t amount_replaced = 0;

  // Repeat until end is reached
  while (pos != string::npos) {
    // Replace this occurrence of sub string
    (*haystack).replace(pos, needle.size(), replacement);

    // Get the next occurrence from the current position
    pos = (*haystack).find(needle, pos + replacement.size());

    ++amount_replaced;
  }

  return amount_replaced;
}

size_t ReplaceAll(string *haystack, const string &needle, const char c) {
  return ReplaceAll(haystack, needle, string({c}));
}

// Split given string by given character delimiter into vector of strings
vector<string> Split(string const &str, char delimiter) {
  vector<string> result;
  std::stringstream iss(str);

  for (string token; getline(iss, token, delimiter);)
    result.push_back(token);

  return result;
}

string Repeat(const string &str, size_t amount) {
  if (amount == 0)
    return "";
  else if (amount == 1)
    return str;

  // Optimize for single-character strings (common case for blockquotes, etc.)
  if (str.size() == 1)
    return string(amount, str[0]);

  // For multi-character strings, reserve space upfront
  string out;
  out.reserve(str.size() * amount);
  for (size_t i = 0; i < amount; ++i)
    out.append(str);

  return out;
}

string toLower(const string &str) {
  string lower;
  lower.reserve(str.size());
  for (char ch : str) {
    lower += tolower(ch);
  }
  return lower;
}

} // namespace

namespace html2md {

Converter::Converter(const string *html, Options *options) : html_(*html) {
  if (options)
    option = *options;

  md_.reserve(html->size() * 1.2);
  tags_.reserve(41);

  // non-printing tags
  auto tagIgnored = make_shared<Converter::TagIgnored>();
  tags_[kTagHead] = tagIgnored;
  tags_[kTagMeta] = tagIgnored;
  tags_[kTagNav] = tagIgnored;
  tags_[kTagNoScript] = tagIgnored;
  tags_[kTagScript] = tagIgnored;
  tags_[kTagStyle] = tagIgnored;
  tags_[kTagTemplate] = tagIgnored;
  // [seimi patch] 新增 iframe：内嵌框架（广告/第三方组件），几乎不含正文，误伤风险极低。
  // 注意：aside/header/footer/form 虽常为非正文，但 HTML5 规范允许它们嵌套在 <article>
  // 内承载正文标题/作者信息/补充说明（如 <article><header><h1>标题</h1></header>）。
  // 按"绝对不误伤正文"原则，不对这些有歧义的标签做整体忽略——宁可保留也不删错。
  tags_[kTagIframe] = tagIgnored;

  // printing tags
  tags_[kTagAnchor] = make_shared<Converter::TagAnchor>();
  tags_[kTagBreak] = make_shared<Converter::TagBreak>();
  tags_[kTagDiv] = make_shared<Converter::TagDiv>();
  tags_[kTagHeader1] = make_shared<Converter::TagHeader1>();
  tags_[kTagHeader2] = make_shared<Converter::TagHeader2>();
  tags_[kTagHeader3] = make_shared<Converter::TagHeader3>();
  tags_[kTagHeader4] = make_shared<Converter::TagHeader4>();
  tags_[kTagHeader5] = make_shared<Converter::TagHeader5>();
  tags_[kTagHeader6] = make_shared<Converter::TagHeader6>();
  tags_[kTagListItem] = make_shared<Converter::TagListItem>();
  tags_[kTagOption] = make_shared<Converter::TagOption>();
  tags_[kTagOrderedList] = make_shared<Converter::TagOrderedList>();
  tags_[kTagPre] = make_shared<Converter::TagPre>();
  tags_[kTagCode] = make_shared<Converter::TagCode>();
  tags_[kTagParagraph] = make_shared<Converter::TagParagraph>();
  tags_[kTagSpan] = make_shared<Converter::TagSpan>();
  tags_[kTagUnorderedList] = make_shared<Converter::TagUnorderedList>();
  tags_[kTagTitle] = make_shared<Converter::TagTitle>();
  tags_[kTagImg] = make_shared<Converter::TagImage>();
  tags_[kTagSeperator] = make_shared<Converter::TagSeperator>();

  // Text formatting
  auto tagBold = make_shared<Converter::TagBold>();
  tags_[kTagBold] = tagBold;
  tags_[kTagStrong] = tagBold;

  auto tagItalic = make_shared<Converter::TagItalic>();
  tags_[kTagItalic] = tagItalic;
  tags_[kTagItalic2] = tagItalic;
  tags_[kTagDefinition] = tagItalic;
  tags_[kTagCitation] = tagItalic;

  tags_[kTagUnderline] = make_shared<Converter::TagUnderline>();

  auto tagStrighthrought = make_shared<Converter::TagStrikethrought>();
  tags_[kTagStrighthrought] = tagStrighthrought;
  tags_[kTagStrighthrought2] = tagStrighthrought;

  tags_[kTagBlockquote] = make_shared<Converter::TagBlockquote>();

  // Tables
  tags_[kTagTable] = make_shared<Converter::TagTable>();
  tags_[kTagTableRow] = make_shared<Converter::TagTableRow>();
  tags_[kTagTableHeader] = make_shared<Converter::TagTableHeader>();
  tags_[kTagTableData] = make_shared<Converter::TagTableData>();
}

void Converter::CleanUpMarkdown() {
  TidyAllLines(&md_);
  std::string buffer;
  buffer.reserve(md_.size());

  // Replace HTML symbols during the initial pass unless the user requested
  // to keep HTML entities intact (e.g. keep `&nbsp;`)
  if (!option.keepHtmlEntities) {
    for (size_t i = 0; i < md_.size();) {
      bool replaced = false;

      // C++11 compatible iteration over htmlSymbolConversions_
      for (const auto &symbol_replacement : htmlSymbolConversions_) {
        const std::string &symbol = symbol_replacement.first;
        const std::string &replacement = symbol_replacement.second;

        if (md_.compare(i, symbol.size(), symbol) == 0) {
          buffer.append(replacement);
          i += symbol.size();
          replaced = true;
          break;
        }
      }

      if (!replaced) {
        buffer.push_back(md_[i++]);
      }
    }
  } else {
    // Keep entities as-is: copy through without transforming
    buffer.append(md_);
  }

  // Use swap instead of move assignment for better pre-C++11 compatibility
  md_.swap(buffer);

  // Optimized replacement sequence
  // Note: Multiple simple passes are faster than one complex pass due to:
  // - Better branch prediction
  // - Better cache locality
  // - Simpler instruction patterns
  const char *replacements[][2] = {
      {" , ", ", "},   {"\n.\n", ".\n"},   {"\n↵\n", " ↵\n"}, {"\n*\n", "\n"},
      {"\n. ", ".\n"}, {"\t\t  ", "\t\t"},
  };

  for (const auto &replacement : replacements) {
    ReplaceAll(&md_, replacement[0], replacement[1]);
  }
}

Converter *Converter::appendToMd(char ch) {
  if (IsInIgnoredTag())
    return this;

  if (index_blockquote != 0 && ch == '\n') {
    if (is_in_pre_) {
      md_ += ch;
      chars_in_curr_line_ = 0;
      appendToMd(Repeat("> ", index_blockquote));
    }

    return this;
  }

  md_ += ch;

  if (ch == '\n')
    chars_in_curr_line_ = 0;
  else
    ++chars_in_curr_line_;

  return this;
}

Converter *Converter::appendToMd(const char *str) {
  if (IsInIgnoredTag())
    return this;

  md_ += str;

  auto str_len = strlen(str);
  
  // Efficiently update chars_in_curr_line_ by scanning for last newline
  for (size_t i = 0; i < str_len; ++i) {
    if (str[i] == '\n')
      chars_in_curr_line_ = 0;
    else
      ++chars_in_curr_line_;
  }

  return this;
}

Converter *Converter::appendBlank() {
  UpdatePrevChFromMd();

  if (prev_ch_in_md_ == '\n' ||
      (prev_ch_in_md_ == '*' && prev_prev_ch_in_md_ == '*'))
    return this;

  return appendToMd(' ');
}

bool Converter::ok() const {
  return !is_in_pre_ && !is_in_list_ && !is_in_p_ && !is_in_table_ &&
         !is_in_tag_ && index_blockquote == 0 && index_li == 0;
}

void Converter::LTrim(string *s) {
  (*s).erase((*s).begin(),
             find_if((*s).begin(), (*s).end(),
                     [](unsigned char ch) { return !std::isspace(ch); }));
}

Converter *Converter::RTrim(string *s, bool trim_only_blank) {
  (*s).erase(find_if((*s).rbegin(), (*s).rend(),
                     [trim_only_blank](unsigned char ch) {
                       if (trim_only_blank)
                         return !isblank(ch);

                       return !isspace(ch);
                     })
                 .base(),
             (*s).end());

  return this;
}

// NOTE: Pay attention when changing one of the trim functions. It can break the
// output!
Converter *Converter::Trim(string *s) {
  if (!startsWith(*s, "\t") || option.forceLeftTrim)
    LTrim(s);

  if (!(startsWith(*s, "  "), endsWith(*s, "  ")))
    RTrim(s);

  return this;
}

void Converter::TidyAllLines(string *str) {
  if (str->empty())
    return;

  // Ensure input ends with newline to simplify logic
  if (str->back() != '\n') {
    str->push_back('\n');
  }

  size_t read = 0;
  size_t write = 0;
  size_t len = str->size();

  uint8_t amount_newlines = 0;
  bool in_code_block = false;

  while (read < len) {
    size_t line_start = read;
    size_t line_end = read;

    // Find end of line
    while (line_end < len && (*str)[line_end] != '\n') {
      line_end++;
    }

    size_t line_len = line_end - line_start;

    // Check for code block markers
    if (line_len >= 3) {
      char c1 = (*str)[line_start];
      char c2 = (*str)[line_start + 1];
      char c3 = (*str)[line_start + 2];
      if ((c1 == '`' && c2 == '`' && c3 == '`') ||
          (c1 == '~' && c2 == '~' && c3 == '~')) {
        in_code_block = !in_code_block;
      }
    }

    if (in_code_block) {
      // Copy line as-is
      if (write != line_start) {
        for (size_t i = 0; i < line_len; ++i) {
          (*str)[write + i] = (*str)[line_start + i];
        }
      }
      write += line_len;
      (*str)[write++] = '\n';
    } else {
      // Trim logic
      size_t trim_start = line_start;
      size_t trim_end = line_end;

      // Trim leading whitespace
      if (option.forceLeftTrim ||
          (trim_start < trim_end && (*str)[trim_start] != '\t')) {
        while (trim_start < trim_end &&
               std::isspace((unsigned char)(*str)[trim_start])) {
          ++trim_start;
        }
      }

      // Trim trailing whitespace, preserve "  "
      bool has_line_break = false;
      if (trim_end >= trim_start + 2 && (*str)[trim_end - 1] == ' ' &&
          (*str)[trim_end - 2] == ' ') {
        has_line_break = true;
        trim_end -= 2;
      }

      while (trim_end > trim_start &&
             std::isspace((unsigned char)(*str)[trim_end - 1])) {
        --trim_end;
      }

      if (has_line_break) {
        trim_end += 2;
      }

      size_t trimmed_len = trim_end - trim_start;

      if (trimmed_len == 0) {
        // Empty line
        if (amount_newlines < 2 && write > 0) {
          (*str)[write++] = '\n';
          amount_newlines++;
        }
      } else {
        amount_newlines = 0;
        if (write != trim_start) {
          for (size_t i = 0; i < trimmed_len; ++i) {
            (*str)[write + i] = (*str)[trim_start + i];
          }
        }
        write += trimmed_len;
        (*str)[write++] = '\n';
      }
    }

    read = line_end + 1;
  }

  str->resize(write);
}

string Converter::ExtractAttributeFromTagLeftOf(const string &attr) {
  // Extract the whole tag from current offset, e.g. from '>', backwards
  auto tag = html_.substr(offset_lt_, index_ch_in_html_ - offset_lt_);
  string lowerTag = toLower(tag); // Convert tag to lowercase for comparison

  // locate given attribute (case-insensitive)
  auto offset_attr = lowerTag.find(attr);

  if (offset_attr == string::npos)
    return "";

  // locate attribute-value pair's '='
  auto offset_equals = tag.find('=', offset_attr);

  if (offset_equals == string::npos)
    return "";

  // locate value's surrounding quotes
  auto offset_double_quote = tag.find('"', offset_equals);
  auto offset_single_quote = tag.find('\'', offset_equals);

  bool has_double_quote = offset_double_quote != string::npos;
  bool has_single_quote = offset_single_quote != string::npos;

  if (!has_double_quote && !has_single_quote)
    return "";

  char wrapping_quote = 0;

  size_t offset_opening_quote = 0;
  size_t offset_closing_quote = 0;

  if (has_double_quote) {
    if (!has_single_quote) {
      wrapping_quote = '"';
      offset_opening_quote = offset_double_quote;
    } else {
      if (offset_double_quote < offset_single_quote) {
        wrapping_quote = '"';
        offset_opening_quote = offset_double_quote;
      } else {
        wrapping_quote = '\'';
        offset_opening_quote = offset_single_quote;
      }
    }
  } else {
    // has only single quote
    wrapping_quote = '\'';
    offset_opening_quote = offset_single_quote;
  }

  if (offset_opening_quote == string::npos)
    return "";

  offset_closing_quote = tag.find(wrapping_quote, offset_opening_quote + 1);

  if (offset_closing_quote == string::npos)
    return "";

  return tag.substr(offset_opening_quote + 1,
                    offset_closing_quote - 1 - offset_opening_quote);
}

void Converter::TurnLineIntoHeader1() {
  appendToMd('\n' + Repeat("=", chars_in_curr_line_) + "\n\n");

  chars_in_curr_line_ = 0;
}

void Converter::TurnLineIntoHeader2() {
  appendToMd('\n' + Repeat("-", chars_in_curr_line_) + "\n\n");

  chars_in_curr_line_ = 0;
}

string Converter::convert() {
  // We already converted
  if (index_ch_in_html_ == html_.size())
    return md_;

  reset();

  for (char ch : html_) {
    ++index_ch_in_html_;

    if (!is_in_tag_ && ch == '<') {
      // [seimi patch] 关键修复：在被忽略标签（script/style/template/noscript/iframe/head/nav）
      // 内部时，'<' 几乎都是 JS/CSS 内容（比较运算 a<b、正则 /</g、字符串 '<div>'、
      // document.write('</scr...') 等），不是真正的标签开始。原逻辑一旦遇到 '<' 就
      // OnHasEnteredTag() 把 current_tag_ 清空，导致 IsInIgnoredTag() 失效，后续 JS
      // 源码会原样泄漏进 markdown（新浪等大站尤其严重）。
      // 修复策略：处于忽略区域时，只有遇到该忽略标签的真正闭合形式 </tagname> 才退出
      // 忽略、走正常标签解析；否则把 '<' 当普通字符消耗掉（什么都不做），保持
      // current_tag_ 不变，确保忽略区一直生效到真正的闭合标签。
      if (IsInIgnoredTag() && !IsIgnoredTagClosingAt(index_ch_in_html_ - 1)) {
        continue;  // 吞掉这个 '<'，不进入标签解析（关键：current_tag_ 保持为忽略标签）
      }

      OnHasEnteredTag();

      continue;
    }

    if (is_in_tag_)
      ParseCharInTag(ch);
    else
      ParseCharInTagContent(ch);
  }

  CleanUpMarkdown();

  // Remove trailing double newline if present (keep only single newline)
  if (md_.size() >= 2 && md_[md_.size() - 1] == '\n' && md_[md_.size() - 2] == '\n') {
    md_.pop_back();
  }

  return md_;
}

void Converter::OnHasEnteredTag() {
  offset_lt_ = index_ch_in_html_;
  is_in_tag_ = true;
  is_closing_tag_ = false;
  prev_tag_ = current_tag_;
  current_tag_ = "";

  if (!md_.empty()) {
    UpdatePrevChFromMd();
  }
}

Converter *Converter::UpdatePrevChFromMd() {
  if (!md_.empty()) {
    prev_ch_in_md_ = md_[md_.length() - 1];

    if (md_.length() > 1)
      prev_prev_ch_in_md_ = md_[md_.length() - 2];
  }

  return this;
}

bool Converter::ParseCharInTag(char ch) {
  static bool skipping_leading_whitespace = true;

  if (ch == '/' && !is_in_attribute_value_) {
    is_closing_tag_ = current_tag_.empty();
    is_self_closing_tag_ = !is_closing_tag_;
    skipping_leading_whitespace = true; // Reset for next tag
    return true;
  }

  if (ch == '>') {
    // Trim trailing whitespace by removing characters from current_tag_
    while (!current_tag_.empty() && std::isspace(current_tag_.back())) {
      current_tag_.pop_back();
    }
    skipping_leading_whitespace = true; // Reset for next tag
    if (!is_self_closing_tag_)
      return OnHasLeftTag();
    else {
      OnHasLeftTag();
      is_self_closing_tag_ = false;
      is_closing_tag_ = true;
      return OnHasLeftTag();
    }
  }

  if (ch == '"') {
    if (is_in_attribute_value_) {
      is_in_attribute_value_ = false;
    } else {
      size_t pos = current_tag_.length();
      while (pos > 0 && isspace(current_tag_[pos - 1])) {
        pos--;
      }
      if (pos > 0 && current_tag_[pos - 1] == '=') {
        is_in_attribute_value_ = true;
      }
    }
    skipping_leading_whitespace = false; // Stop skipping after attribute
    return true;
  }

  // Handle whitespace: skip leading whitespace, keep others
  if (isspace(ch) && skipping_leading_whitespace) {
    return true; // Ignore leading whitespace
  }

  // Once we encounter a non-whitespace character, stop skipping
  skipping_leading_whitespace = false;
  current_tag_ += tolower(ch);
  return false;
}

bool Converter::OnHasLeftTag() {
  is_in_tag_ = false;

  UpdatePrevChFromMd();

  if (!is_closing_tag_)
    if (TagContainsAttributesToHide(&current_tag_))
      return true;

  // Extract tag name without Split() - just find first space
  size_t space_pos = current_tag_.find(' ');
  if (space_pos != string::npos) {
    current_tag_ = current_tag_.substr(0, space_pos);
  }
  
  if (current_tag_.empty())
    return true;

  auto tag = tags_[current_tag_];

  if (!tag)
    return true;

  if (!is_closing_tag_) {
    tag->OnHasLeftOpeningTag(this);
  }
  else {
    is_closing_tag_ = false;

    tag->OnHasLeftClosingTag(this);

    // [seimi patch] 关闭标签处理完后，current_tag_ 仍是该标签名（如 "script"）。
    // 对忽略标签来说这会让 IsInIgnoredTag() 继续返回 true，把后续正文也吃掉。
    // 必须复位为一个非忽略的占位（空串即可），让忽略区真正结束。
    // 注意：这与 convert() 主循环的 [seimi patch] 配套——主循环靠 IsIgnoredTagClosingAt
    // 识别 </script> 才放行标签解析，所以能走到这里说明我们刚处理完一个真正的闭合标签。
    if (IsIgnoredTag(current_tag_))
      current_tag_.clear();
  }

  return true;
}

Converter *Converter::ShortenMarkdown(size_t chars) {
  md_ = md_.substr(0, md_.length() - chars);

  if (chars > chars_in_curr_line_)
    chars_in_curr_line_ = 0;
  else
    chars_in_curr_line_ = chars_in_curr_line_ - chars;

  return this->UpdatePrevChFromMd();
}

bool Converter::ParseCharInTagContent(char ch) {
  if (is_in_code_) {
    md_ += ch;

    if (index_blockquote != 0 && ch == '\n')
      appendToMd(Repeat("> ", index_blockquote));

    return true;
  }

  if (option.compressWhitespace && !is_in_pre_) {
    if (ch == '\t')
      ch = ' ';

    if (ch == ' ') {
      UpdatePrevChFromMd();
      if (prev_ch_in_md_ == ' ' || prev_ch_in_md_ == '\n')
        return true;
    }
  }

  if (IsInIgnoredTag() || current_tag_ == kTagLink) {
    prev_ch_in_html_ = ch;

    return true;
  }

  if (ch == '\n') {
    // [seimi patch] 链接内的裸 '\n' 一律吞掉（不写进 md）。
    // 原逻辑在 blockquote 里会把 '\n' 写出并补 '> ' 前缀——这在锚点内会破坏
    // [ ... ](href) 结构（即使我们抑制了 blockquote 开标签的前缀，这里的
    // '\n' 仍会泄入）。锚点内链接文字必须单行，所有换行来源都要堵住。
    if (IsInAnchor())
      return true;

    if (index_blockquote != 0) {
      md_ += '\n';
      chars_in_curr_line_ = 0;
      appendToMd(Repeat("> ", index_blockquote));
    }

    return true;
  }

  if (ch == '\r' && IsInAnchor()) {
    // [seimi patch] 链接内的 '\r' 一律吞掉（不写进 md）。
    // 网页 HTML 的换行常是 CRLF（\r\n）。上面只拦了 '\n'，'\r' 会落到 default
    // 分支被原样写进 md，破坏 [ ... ](href) 结构（实测 bing 搜索结果页链接文本
    // 被 \r 拆碎）。锚点内吞掉；锚点外保持原样（不改 html2md 对 \r 的既有行为，
    // 避免影响非链接内容的输出）。
    return true;
  }

  switch (ch) {
  case '*':
    appendToMd("\\*");
    break;
  case '`':
    appendToMd("\\`");
    break;
  case '\\':
    appendToMd("\\\\");
    break;
  case '.': {
    bool is_ordered_list_start = false;
    if (chars_in_curr_line_ > 0) {
      size_t start_idx = md_.length() - chars_in_curr_line_;
      size_t idx = start_idx;
      // Skip spaces
      while (idx < md_.length() && isspace(md_[idx])) {
        idx++;
      }
      // Check digits
      bool has_digits = false;
      while (idx < md_.length() && isdigit(md_[idx])) {
        has_digits = true;
        idx++;
      }
      // If we reached the end and had digits, it's a match
      if (has_digits && idx == md_.length()) {
        is_ordered_list_start = true;
      }
    }

    if (is_ordered_list_start && option.escapeNumberedList) {
      appendToMd("\\.");
    } else {
      md_ += ch;
      ++chars_in_curr_line_;
    }
    break;
  }
  default:
    md_ += ch;
    ++chars_in_curr_line_;
    break;
  }

  if (chars_in_curr_line_ > option.softBreak && !is_in_table_ && !is_in_list_ &&
      current_tag_ != kTagImg && current_tag_ != kTagAnchor &&
      option.splitLines) {
    // [seimi patch] 链接文本内（含祖先 <a>）绝不 soft/hard wrap：
    // 换行会破坏 [ ... ](href) 结构。current_tag_ != kTagAnchor 只挡住"直接"
    // 在 <a> 文本节点的情况，挡不住 <a><div>长文本</div></a> 这种结构——
    // div 被压平后 current_tag_ 是 div，long text 仍会触发 hardBreak → 破坏链接。
    // 用 IsInAnchor() 覆盖所有祖先锚点。
    if (IsInAnchor())
      return false;

    if (ch == ' ') { // If the next char is - it will become a list
      md_ += '\n';
      chars_in_curr_line_ = 0;
    } else if (chars_in_curr_line_ > option.hardBreak) {
      ReplacePreviousSpaceInLineByNewline();
    }
  }

  return false;
}

bool Converter::ReplacePreviousSpaceInLineByNewline() {
  if (current_tag_ == kTagParagraph ||
      is_in_table_ && (prev_tag_ != kTagCode && prev_tag_ != kTagPre))
    return false;

  auto offset = md_.length() - 1;

  if (md_.length() == 0)
    return true;

  do {
    if (md_[offset] == '\n')
      return false;

    if (md_[offset] == ' ') {
      md_[offset] = '\n';
      chars_in_curr_line_ = md_.length() - offset;

      return true;
    }

    --offset;
  } while (offset > 0);

  return false;
}

void Converter::TagAnchor::OnHasLeftOpeningTag(Converter *c) {
  if (c->prev_tag_ == kTagImg)
    c->appendToMd('\n');

  // [seimi patch] 进入锚点：记深度（含非法嵌套 <a>）。块级标签开标签据此
  // 抑制前导换行，避免换行掉进 [ ... ](href) 链接文本。
  // 注意：在写 '[' 之前计数——这样新计数对 '[' 后紧跟的块标签立即生效。
  ++c->is_in_anchor_;

  current_title_ = c->ExtractAttributeFromTagLeftOf(kAttributeTitle);

  c->appendToMd('[');
  current_href_ = c->ExtractAttributeFromTagLeftOf(kAttributeHref);
}

void Converter::TagAnchor::OnHasLeftClosingTag(Converter *c) {
  // [seimi patch] 无论链接文本是否为空，都要退出锚点深度计数（保护下界）。
  if (c->is_in_anchor_ > 0)
    --c->is_in_anchor_;

  if (!c->shortIfPrevCh('[')) {
    // [seimi patch] 链接文本尾部清理：锚点内块标签被压平后可能残留尾空白
    // （如 <a><div>text </div></a> 里的尾空格），RTrim 掉避免 [text ](url)。
    // 在写 ']' 之前做；只 trim md 尾部的空格/制表符，不动换行。
    c->RTrimAnchorTrailingWs();

    c->appendToMd("](")->appendToMd(current_href_);

    // If title is set append it
    if (!current_title_.empty()) {
      c->appendToMd(" \"")->appendToMd(current_title_)->appendToMd('"');
      current_title_.clear();
    }

    c->appendToMd(')');

    if (c->prev_tag_ == kTagImg)
      c->appendToMd('\n');
  }
}

// [seimi patch] 去掉 md 尾部连续的空白字符（空格/制表符），用于锚点关闭前
// 清理链接文本尾部，避免出现 [text ](url)。不动换行符（换行根本不该出现在
// 链接文本里——由块标签抑制保证；若有残留换行，CleanUpMarkdown 兜底）。
// 借 ShortenMarkdown 复用 prev_ch 更新逻辑。声明为 private 成员（与 RTrim 同区）。
void Converter::RTrimAnchorTrailingWs() {
  while (!md_.empty()) {
    char last = md_[md_.length() - 1];
    if (last == ' ' || last == '\t') {
      ShortenMarkdown(1);
    } else {
      break;
    }
  }
}

void Converter::TagBold::OnHasLeftOpeningTag(Converter *c) {
  c->appendToMd("**");
}

void Converter::TagBold::OnHasLeftClosingTag(Converter *c) {
  c->appendToMd("**");
}

void Converter::TagItalic::OnHasLeftOpeningTag(Converter *c) {
  c->appendToMd('*');
}

void Converter::TagItalic::OnHasLeftClosingTag(Converter *c) {
  c->appendToMd('*');
}

void Converter::TagUnderline::OnHasLeftOpeningTag(Converter *c) {
  c->appendToMd("<u>");
}

void Converter::TagUnderline::OnHasLeftClosingTag(Converter *c) {
  c->appendToMd("</u>");
}

void Converter::TagStrikethrought::OnHasLeftOpeningTag(Converter *c) {
  c->appendToMd('~');
}

void Converter::TagStrikethrought::OnHasLeftClosingTag(Converter *c) {
  c->appendToMd('~');
}

void Converter::TagBreak::OnHasLeftOpeningTag(Converter *c) {
  if (c->is_in_list_) { // When it's in a list, it's not in a paragraph
    c->appendToMd("  \n");
    c->appendToMd(Repeat("  ", c->index_li));
  } else if (c->is_in_table_) {
    c->appendToMd("<br>");
  } else if (c->IsInAnchor()) {
    // [seimi patch] 链接内的 <br> 不能换行（会破坏 [ ... ](href) 结构），
    // 降级为单个空格，把 <br> 两侧的文字粘到一行。
    c->appendToMd(' ');
  } else if (!c->md_.empty())
    c->appendToMd("  \n");
}

void Converter::TagBreak::OnHasLeftClosingTag(Converter *c) {}

void Converter::TagDiv::OnHasLeftOpeningTag(Converter *c) {
  // [seimi patch] 链接内的 <div> 不吐换行（会破坏 [ ... ](href) 结构）。
  // 现代网页（搜索结果卡片等）常把 <div> 包在 <a> 内承载链接文字。
  if (c->IsInAnchor())
    return;

  if (c->prev_ch_in_md_ != '\n')
    c->appendToMd('\n');

  if (c->prev_prev_ch_in_md_ != '\n')
    c->appendToMd('\n');
}

void Converter::TagDiv::OnHasLeftClosingTag(Converter *c) {}

void Converter::TagHeader1::OnHasLeftOpeningTag(Converter *c) {
  // [seimi patch] 链接内的标题不能产生 '#'/换行（破坏 [ ... ](href) 结构，
  // 且 markdown 链接文本里放 '#' 大多数渲染器不识别）。降级为加粗。
  if (c->IsInAnchor()) {
    c->appendToMd("**");
    return;
  }
  c->appendToMd("\n# ");
}

void Converter::TagHeader1::OnHasLeftClosingTag(Converter *c) {
  // [seimi patch] 链接内标题降级为加粗，关闭时配对输出 '**'。
  if (c->IsInAnchor()) {
    c->appendToMd("**");
    return;
  }
  if (c->prev_prev_ch_in_md_ != ' ')
    c->appendToMd('\n');
}

void Converter::TagHeader2::OnHasLeftOpeningTag(Converter *c) {
  // [seimi patch] 见 TagHeader1。
  if (c->IsInAnchor()) {
    c->appendToMd("**");
    return;
  }
  c->appendToMd("\n## ");
}

void Converter::TagHeader2::OnHasLeftClosingTag(Converter *c) {
  // [seimi patch] 见 TagHeader1。
  if (c->IsInAnchor()) {
    c->appendToMd("**");
    return;
  }
  if (c->prev_prev_ch_in_md_ != ' ')
    c->appendToMd('\n');
}

void Converter::TagHeader3::OnHasLeftOpeningTag(Converter *c) {
  // [seimi patch] 见 TagHeader1。
  if (c->IsInAnchor()) {
    c->appendToMd("**");
    return;
  }
  c->appendToMd("\n### ");
}

void Converter::TagHeader3::OnHasLeftClosingTag(Converter *c) {
  // [seimi patch] 见 TagHeader1。
  if (c->IsInAnchor()) {
    c->appendToMd("**");
    return;
  }
  if (c->prev_prev_ch_in_md_ != ' ')
    c->appendToMd('\n');
}

void Converter::TagHeader4::OnHasLeftOpeningTag(Converter *c) {
  // [seimi patch] 见 TagHeader1。
  if (c->IsInAnchor()) {
    c->appendToMd("**");
    return;
  }
  c->appendToMd("\n#### ");
}

void Converter::TagHeader4::OnHasLeftClosingTag(Converter *c) {
  // [seimi patch] 见 TagHeader1。
  if (c->IsInAnchor()) {
    c->appendToMd("**");
    return;
  }
  if (c->prev_prev_ch_in_md_ != ' ')
    c->appendToMd('\n');
}

void Converter::TagHeader5::OnHasLeftOpeningTag(Converter *c) {
  // [seimi patch] 见 TagHeader1。
  if (c->IsInAnchor()) {
    c->appendToMd("**");
    return;
  }
  c->appendToMd("\n##### ");
}

void Converter::TagHeader5::OnHasLeftClosingTag(Converter *c) {
  // [seimi patch] 见 TagHeader1。
  if (c->IsInAnchor()) {
    c->appendToMd("**");
    return;
  }
  if (c->prev_prev_ch_in_md_ != ' ')
    c->appendToMd('\n');
}

void Converter::TagHeader6::OnHasLeftOpeningTag(Converter *c) {
  // [seimi patch] 见 TagHeader1。
  if (c->IsInAnchor()) {
    c->appendToMd("**");
    return;
  }
  c->appendToMd("\n###### ");
}

void Converter::TagHeader6::OnHasLeftClosingTag(Converter *c) {
  // [seimi patch] 见 TagHeader1。
  if (c->IsInAnchor()) {
    c->appendToMd("**");
    return;
  }
  if (c->prev_prev_ch_in_md_ != ' ')
    c->appendToMd('\n');
}

void Converter::TagListItem::OnHasLeftOpeningTag(Converter *c) {
  if (c->is_in_table_)
    return;

  if (!c->is_in_ordered_list_) {
    c->appendToMd(string({c->option.unorderedList, ' '}));
    return;
  }

  ++c->index_ol;

  string num = std::to_string(c->index_ol);
  num.append({c->option.orderedList, ' '});
  c->appendToMd(num);
}

void Converter::TagListItem::OnHasLeftClosingTag(Converter *c) {
  if (c->is_in_table_)
    return;

  // [seimi patch] 链接内的 <li> 关闭不换行（破坏 [ ... ](href) 结构）。
  if (c->IsInAnchor())
    return;

  if (c->prev_ch_in_md_ != '\n')
    c->appendToMd('\n');
}

void Converter::TagOption::OnHasLeftOpeningTag(Converter *c) {}

void Converter::TagOption::OnHasLeftClosingTag(Converter *c) {
  if (c->md_.length() == 0)
    return;
  // [seimi patch] 链接内 <option> 关闭不换行（破坏 [ ... ](href) 结构）。
  if (c->IsInAnchor())
    return;
  c->appendToMd("  \n");
}

void Converter::TagOrderedList::OnHasLeftOpeningTag(Converter *c) {
  if (c->is_in_table_)
    return;

  c->is_in_list_ = true;
  c->is_in_ordered_list_ = true;
  c->index_ol = 0;

  ++c->index_li;

  // [seimi patch] 链接内的列表：仍进 list 状态（让 <li> 的 "1. " 生成），
  // 但不 ReplacePreviousSpaceInLineByNewline（会把空格改成 '\n'，破坏 [ ... ](href)），
  // 也不吐前导 '\n'。
  if (!c->IsInAnchor())
    c->ReplacePreviousSpaceInLineByNewline();

  if (!c->IsInAnchor())
    c->appendToMd('\n');
}

void Converter::TagOrderedList::OnHasLeftClosingTag(Converter *c) {
  if (c->is_in_table_)
    return;

  c->is_in_ordered_list_ = false;

  if (c->index_li != 0)
    --c->index_li;

  c->is_in_list_ = c->index_li != 0;

  // [seimi patch] 链接内的列表关闭不换行。
  if (!c->IsInAnchor())
    c->appendToMd('\n');
}

void Converter::TagParagraph::OnHasLeftOpeningTag(Converter *c) {
  c->is_in_p_ = true;

  // [seimi patch] 链接内的 <p> 不吐前导换行（破坏 [ ... ](href) 结构）。
  if (c->IsInAnchor())
    return;

  if (c->is_in_list_ && c->prev_tag_ == kTagParagraph)
    c->appendToMd("\n\t");
  else if (!c->is_in_list_)
    c->appendToMd('\n');
}

void Converter::TagParagraph::OnHasLeftClosingTag(Converter *c) {
  c->is_in_p_ = false;

  // [seimi patch] 链接内的 <p> 关闭也不换行。
  if (c->IsInAnchor())
    return;

  if (!c->md_.empty())
    c->appendToMd("\n"); // Workaround \n restriction for blockquotes

  if (c->index_blockquote != 0)
    c->appendToMd(Repeat("> ", c->index_blockquote));
}

void Converter::TagPre::OnHasLeftOpeningTag(Converter *c) {
  c->is_in_pre_ = true;

  // [seimi patch] 链接内的 <pre> 不吐前导换行（破坏 [ ... ](href) 结构），
  // 但仍保留 ``` 围栏以维持 pre 内容格式。pre 内容内的裸换行不在本 patch 范围
  // （<a> 内嵌 <pre> 极罕见；多数网页不会这么写）。
  if (!c->IsInAnchor()) {
    if (c->prev_ch_in_md_ != '\n')
      c->appendToMd('\n');

    if (c->prev_prev_ch_in_md_ != '\n')
      c->appendToMd('\n');
  }

  if (c->is_in_list_ && c->prev_tag_ != kTagParagraph)
    c->ShortenMarkdown(2);

  if (c->is_in_list_)
    c->appendToMd("\t\t");
  else
    c->appendToMd("```");
}

void Converter::TagPre::OnHasLeftClosingTag(Converter *c) {
  c->is_in_pre_ = false;

  if (c->is_in_list_)
    return;

  c->appendToMd("```");

  // [seimi patch] 链接内 <pre> 关闭不换行。
  if (!c->IsInAnchor())
    c->appendToMd('\n'); // Don't combine because of blockquote
}

void Converter::TagCode::OnHasLeftOpeningTag(Converter *c) {
  c->is_in_code_ = true;

  if (c->is_in_pre_) {
    if (c->is_in_list_)
      return;

    auto code = c->ExtractAttributeFromTagLeftOf(kAttributeClass);
    if (!code.empty()) {
      if (startsWith(code, "language-"))
        code.erase(0, 9); // remove language-
      c->appendToMd(code);
    }
    c->appendToMd('\n');
  } else
    c->appendToMd('`');
}

void Converter::TagCode::OnHasLeftClosingTag(Converter *c) {
  c->is_in_code_ = false;

  if (c->is_in_pre_)
    return;

  c->appendToMd('`');
}

void Converter::TagSpan::OnHasLeftOpeningTag(Converter *c) {}

void Converter::TagSpan::OnHasLeftClosingTag(Converter *c) {}

void Converter::TagTitle::OnHasLeftOpeningTag(Converter *c) {}

void Converter::TagTitle::OnHasLeftClosingTag(Converter *c) {
  c->TurnLineIntoHeader1();
}

void Converter::TagUnorderedList::OnHasLeftOpeningTag(Converter *c) {
  if (c->is_in_list_ || c->is_in_table_)
    return;

  c->is_in_list_ = true;

  ++c->index_li;

  // [seimi patch] 链接内的列表：仍进 list 状态（让 <li> 的 "- " 生成），
  // 但不吐前导 '\n'（破坏 [ ... ](href) 结构）。
  if (!c->IsInAnchor())
    c->appendToMd('\n');
}

void Converter::TagUnorderedList::OnHasLeftClosingTag(Converter *c) {
  if (c->is_in_table_)
    return;

  if (c->index_li != 0)
    --c->index_li;

  c->is_in_list_ = c->index_li != 0;

  // [seimi patch] 链接内的列表关闭不换行。
  if (c->IsInAnchor())
    return;

  if (c->prev_prev_ch_in_md_ == '\n' && c->prev_ch_in_md_ == '\n')
    c->ShortenMarkdown();
  else if (c->prev_ch_in_md_ != '\n')
    c->appendToMd('\n');
}

void Converter::TagImage::OnHasLeftOpeningTag(Converter *c) {
  // [seimi patch] 图片在锚点内时不吐前导换行（会破坏 [ ... ](href) 链接结构）。
  // 原条件 prev_tag_ != kTagAnchor 只挡住"直接"紧跟在 <a> 后的图片，
  // 挡不住 <a><div><img/></div></a> 这种结构（prev_tag_ 是 div）。
  // 用 IsInAnchor() 覆盖所有祖先锚点。
  if (!c->IsInAnchor() && c->prev_tag_ != kTagAnchor &&
      c->prev_ch_in_md_ != '\n')
    c->appendToMd('\n');

  c->appendToMd("![")
      ->appendToMd(c->ExtractAttributeFromTagLeftOf(kAttributeAlt))
      ->appendToMd("](")
      ->appendToMd(c->ExtractAttributeFromTagLeftOf(kAttributeSrc));

  auto title = c->ExtractAttributeFromTagLeftOf(kAttributeTitle);
  if (!title.empty()) {
    c->appendToMd(" \"")->appendToMd(title)->appendToMd('"');
  }

  c->appendToMd(")");
}

void Converter::TagImage::OnHasLeftClosingTag(Converter *c) {
  // [seimi patch] 图片在锚点内关闭时不吐换行（破坏 [ ... ](href) 结构）。
  // 原条件 prev_tag_ == kTagAnchor 同样只覆盖"直接"父级是 <a> 的情况。
  if (!c->IsInAnchor() && c->prev_tag_ == kTagAnchor)
    c->appendToMd('\n');
}

void Converter::TagSeperator::OnHasLeftOpeningTag(Converter *c) {
  // [seimi patch] 链接内的 <hr> 不能产生 "\n---\n"（破坏 [ ... ](href) 结构，
  // 且 --- 在 markdown 里是主题分隔线）。降级为单个破折号 " — "，作为视觉分隔占位。
  if (c->IsInAnchor()) {
    c->appendToMd(" — ");
    return;
  }
  c->appendToMd("\n---\n"); // NOTE: We can make this an option
}

void Converter::TagSeperator::OnHasLeftClosingTag(Converter *c) {}

void Converter::TagTable::OnHasLeftOpeningTag(Converter *c) {
  c->is_in_table_ = true;
  // [seimi patch] 链接内的 <table> 不吐前导换行（破坏 [ ... ](href) 结构）。
  // table_start 仍记录当前 md 末尾位置，作为 CloseTag 里表格格式化的起点。
  if (!c->IsInAnchor())
    c->appendToMd('\n');
  c->table_start = c->md_.length(); // Set start AFTER the (optional) newline
}

void Converter::TagTable::OnHasLeftClosingTag(Converter *c) {
  c->is_in_table_ = false;

  // [seimi patch] 链接内的 <table> 关闭不换行；且跳过 formatMarkdownTable 后处理
  // （它会重排 table_start 到 md 末尾，把链接的 ] ... ) 也吞进去重排，彻底破坏链接）。
  if (c->IsInAnchor())
    return;

  c->appendToMd('\n');

  if (!c->option.formatTable)
    return;

  string table = c->md_.substr(c->table_start);
  table = formatMarkdownTable(table);
  c->ShortenMarkdown(c->md_.size() - c->table_start);
  c->appendToMd(table);
}

void Converter::TagTableRow::OnHasLeftOpeningTag(Converter *c) {
  // Don't add newline here - it creates empty rows
  // The newline is added by the closing tag of the previous row
}

void Converter::TagTableRow::OnHasLeftClosingTag(Converter *c) {
  c->UpdatePrevChFromMd();

  // Always close the row with a pipe and space, then newline
  if (c->prev_ch_in_md_ != '|') {
    c->appendToMd(" |");
  }
  // [seimi patch] 链接内表格行不换行（破坏 [ ... ](href) 结构）。
  // <table> 包在 <a> 内极罕见，此处保证至少不吐换行；表格分隔线行
  // （tableLine）在锚点内也一并跳过，避免 "|---|" 残留。
  if (c->IsInAnchor())
    return;

  c->appendToMd('\n');

  if (!c->tableLine.empty()) {
    c->tableLine.append("|\n");
    c->appendToMd(c->tableLine);
    c->tableLine.clear();
  }
}


void Converter::TagTableHeader::OnHasLeftOpeningTag(Converter *c) {
  auto align = c->ExtractAttributeFromTagLeftOf(kAttrinuteAlign);

  string line = "| ";

  if (align == "left" || align == "center")
    line += ':';

  line += '-';

  if (align == "right" || align == "center")
    line += ": ";
  else
    line += ' ';

  c->tableLine.append(line);

  c->appendToMd("| ");
}

void Converter::TagTableHeader::OnHasLeftClosingTag(Converter *c) {
  c->appendToMd(" ");
}


void Converter::TagTableData::OnHasLeftOpeningTag(Converter *c) {
  c->appendToMd("| ");
}


void Converter::TagTableData::OnHasLeftClosingTag(Converter *c) {
  c->appendToMd(" ");
}


void Converter::TagBlockquote::OnHasLeftOpeningTag(Converter *c) {
  ++c->index_blockquote;
  // [seimi patch] 链接内的 <blockquote> 不吐前导换行与 '> ' 前缀（破坏 [ ... ](href) 结构）。
  // index_blockquote 仍递增以保证开闭配对；CloseTag 仅在 md 末尾恰为 '> ' 时 shorten，
  // 而锚点内我们没写 '> '，故 close 是无副作用 no-op。
  if (c->IsInAnchor())
    return;
  c->appendToMd("\n");
  c->appendToMd(Repeat("> ", c->index_blockquote));
}

void Converter::TagBlockquote::OnHasLeftClosingTag(Converter *c) {
  --c->index_blockquote;
  // Only shorten if a "> " was added (i.e., a newline was processed in the blockquote)
  if (!c->md_.empty() && c->md_.length() >= 2 &&
      c->md_.substr(c->md_.length() - 2) == "> ") {
    c->ShortenMarkdown(2); // Remove the '> ' only if it exists
  }
}

void Converter::reset() {
  md_.clear();
  prev_ch_in_md_ = 0;
  prev_prev_ch_in_md_ = 0;
  index_ch_in_html_ = 0;
  // [seimi patch] 复位锚点深度计数（convert() 每次开始前会 reset）。
  is_in_anchor_ = 0;
}

bool Converter::IsInIgnoredTag() const {
  if (current_tag_ == kTagTitle && !option.includeTitle)
    return true;

  return IsIgnoredTag(current_tag_);
}

// [seimi patch] 检查从 pos 起的 HTML 是否是 "被忽略标签的闭合形式"。
// 形如 </script、</style、</template、</noscript、</iframe、</head、</nav、</meta
// （</scr 之类不完整写法视为非闭合：必须完整匹配标签名，避免误判脚本里 a</b 之类）。
// 大小写不敏感；不要求后面紧跟 '>'（属性/空白/换行后跟 '>' 也算）。
// 实现要点：用一个 8 字节定长 buffer 收集 '</' 之后的字符再 compare，避免子串分配。
bool Converter::IsIgnoredTagClosingAt(size_t pos) const {
  // 至少要有 "</" + 标签名（最短 "meta" 是 4 字符）= 6 字节。
  if (pos + 6 > html_.size())
    return false;

  // pos 指向 '<'，pos+1 必须是 '/'。
  if (html_[pos] != '<' || html_[pos + 1] != '/')
    return false;

  // 收集 '/' 之后最多 8 字节（到空白/'>'为止）。所有忽略标签名 ≤ 8 字符
  //（template 是 8，正好够）。同时小写化，便于和 ASCII 小写的常量比较。
  char buf[9] = {0};
  size_t blen = 0;
  for (size_t i = pos + 2; i < html_.size() && blen < 8; ++i) {
    char c = html_[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '>')
      break;
    buf[blen++] = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (blen == 0)
    return false;

  // 与所有忽略标签名逐一比对（已在上面转小写，常量本身也都是 ASCII 小写）。
  static const char *kIgnored[] = {kTagScript, kTagStyle, kTagTemplate,
                                   kTagNoScript, kTagIframe, kTagHead,
                                   kTagNav, kTagMeta};
  for (const char *t : kIgnored) {
    size_t tlen = std::strlen(t);
    if (blen == tlen && std::memcmp(buf, t, tlen) == 0)
      return true;
  }
  return false;
}
} // namespace html2md
