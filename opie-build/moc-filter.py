#!/usr/bin/env python3
"""Filter C++ source for moc compatibility with GCC 12 system headers.
Strategy: strip system includes and replace all function bodies with { },
recursively processing class bodies to clean their member functions too."""
import re, sys

def clean_bodies(text):
    """Replace function/constructor bodies with { }, keeping class/struct bodies."""
    result = []
    i = 0
    while i < len(text):
        if text[i] == '{':
            # Find matching close brace
            depth = 1
            j = i + 1
            while j < len(text) and depth > 0:
                if text[j] == '{': depth += 1
                elif text[j] == '}': depth -= 1
                j += 1
            body_inner = text[i+1:j-1]  # content between braces

            # Is this a class/struct body?
            has_class_keywords = bool(re.search(
                r'Q_OBJECT|public\s*:|private\s*:|protected\s*:|signals\s*:|slots\s*:',
                body_inner))

            if has_class_keywords:
                # Recursively clean the class body contents
                cleaned = clean_bodies(body_inner)
                result.append('{')
                result.append(cleaned)
                result.append('}')
            else:
                # Replace with empty body
                result.append('{ }')
            i = j
        else:
            result.append(text[i])
            i += 1
    return ''.join(result)

text = open(sys.argv[1]).read()

# 1. Comment out system includes
text = re.sub(r'^#include\s*<[^>]+>', '/* moc-stub */', text, flags=re.MULTILINE)

# 2. Replace ALL brace-delimited blocks (repeatedly until no more nested ones)
while True:
    new = re.sub(r'\{[^{}]+\}', '{ }', text)
    if new == text: break
    text = new

# 3. Remove constructor initializer lists: Ctor(args) : Base(args) { }
# Only match when { } appears on the same logical block (not across class boundaries)
# Pattern: ) : stuff { } where "stuff" doesn't contain class keywords
def strip_init_lists(text):
    # Match: ) followed by : and stuff (no class keywords) then { }
    # Use a line-by-line approach to avoid greedy cross-class matching
    lines = text.split('\n')
    result = []
    i = 0
    while i < len(lines):
        line = lines[i]
        # Check for constructor init list: line ending with ) or containing ) :
        # followed by lines with init args, then { }
        if re.search(r'\)\s*:\s*\w', line) or (result and re.search(r'\)\s*$', result[-1] if result else '')):
            # Collect lines until we find { }
            collected = [line]
            j = i + 1
            while j < len(lines) and '{ }' not in lines[j] and '{' not in lines[j]:
                collected.append(lines[j])
                j += 1
            if j < len(lines) and '{ }' in lines[j]:
                collected.append(lines[j])
                # Check if this looks like a constructor init list (no class keywords)
                block = '\n'.join(collected)
                if not any(kw in block for kw in ['class ', 'struct ', 'public:', 'private:', 'protected:', 'Q_OBJECT']):
                    # Find the Ctor(args) part and replace
                    ctor_match = re.match(r'(.*\))\s*:', collected[0])
                    if ctor_match:
                        result.append(ctor_match.group(1) + ' { }')
                        i = j + 1
                        continue
        result.append(line)
        i += 1
    return '\n'.join(result)

text = strip_init_lists(text)

# 4. Replace template angle brackets in member declarations with moc-safe versions
# QPtrDict<Type> * name -> void * name  (moc doesn't care about types)
text = re.sub(r'\w+<[^>]+>\s*\*', 'void *', text)
text = re.sub(r'\w+<[^>]+>\s*&', 'void &', text)

# 5. Clean recursively one more time
text = clean_bodies(text)

open(sys.argv[2], 'w').write(text)
