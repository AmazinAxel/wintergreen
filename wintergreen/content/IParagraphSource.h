#pragma once

#include <cstddef>

#include "ContentModel.h"

namespace wintergreen {

// Abstract interface for accessing paragraphs.
// Lets TextLayout work with in-memory Chapter or on-disk WGB files.
struct IParagraphSource {
  virtual ~IParagraphSource() = default;
  virtual size_t paragraph_count() const = 0;
  virtual const Paragraph& paragraph(size_t index) const = 0;
  // Whether paragraph 0 is the first of its spine file. Only that paragraph's
  // spacing_before is kept at the top of a page (see TextLayout's
  // leading_spacer); a page source that starts later must say so, or its first
  // paragraph's index space shifts by one against the page before it.
  virtual bool starts_unit() const {
    return true;
  }
};

}  // namespace wintergreen
