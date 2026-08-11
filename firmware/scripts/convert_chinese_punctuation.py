#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Chinese to English Punctuation Converter
Converts all Chinese punctuation in a text file to English equivalents.

Usage:
    python convert_chinese_punctuation.py input.txt [output.txt]
    
If output.txt is not specified, the script will overwrite input.txt
"""

import sys
import os

# Chinese punctuation mapping to English equivalents
CHINESE_TO_ENGLISH_PUNCTUATION = {
    # Basic punctuation
    '\uff0c': ',',      # ， Chinese comma -> English comma
    '\u3001': ',',      # 、 Chinese enumeration comma -> English comma
    '\uff1b': ';',      # ； Chinese semicolon -> English semicolon
    '\uff1a': ':',      # ： Chinese colon -> English colon
    '\uff1f': '?',      # ？ Chinese question mark -> English question mark
    '\uff01': '!',      # ！ Chinese exclamation mark -> English exclamation mark
    
    # Quotation marks (using Unicode escapes to avoid syntax issues)
    '\u201c': '\u300c',      # " Left double quotation mark
    '\u201d': '\u300d',      # " Right double quotation mark
    '\u2018': "'",      # ' Left single quotation mark
    '\u2019': "'",      # ' Right single quotation mark
    
    # Brackets and parentheses
    '\uff08': '(',      # （ Fullwidth left parenthesis
    '\uff09': ')',      # ） Fullwidth right parenthesis
    '\u3010': '[',      # 【 Left black lenticular bracket
    '\u3011': ']',      # 】 Right black lenticular bracket
    '\u3014': '[',      # 〔 Left tortoise shell bracket
    '\u3015': ']',      # 〕 Right tortoise shell bracket
    '\u3016': '[',      # 〖 Left white lenticular bracket
    '\u3017': ']',      # 〗 Right white lenticular bracket
    '\u300a': '<',      # 《 Left angle bracket
    '\u300b': '>',      # 》 Right angle bracket
    '\u3008': '<',      # 〈 Left angle bracket
    '\u3009': '>',      # 〉 Right angle bracket
    
    # Other symbols
    '\u2026': '...',    # … Horizontal ellipsis
    '\u22ef': '...',    # ⋯ Midline horizontal ellipsis
    '\u2014': '--',     # — Em dash
    '\uff5e': '~',      # ～ Fullwidth tilde
    '\u301c': '~',      # 〜 Wave dash
    '\u30fb': '\xb7',   # ・ Katakana middle dot -> Middle dot
    '\u00b7': '\xb7',   # · Middle dot (keep)
    '\u2022': '\u2022', # • Bullet (keep)
    '\u2605': '*',      # ★ Black star
    '\u2606': '*',      # ☆ White star
    '\u25cb': 'O',      # ○ White circle
    '\u25cf': 'O',      # ● Black circle
}

# Fullwidth ASCII to ASCII mapping
FULLWIDTH_TO_ASCII = {}
for i in range(10):
    FULLWIDTH_TO_ASCII[chr(0xFF10 + i)] = chr(0x30 + i)  # Fullwidth digits 0-9

# Fullwidth letters
for i in range(26):
    FULLWIDTH_TO_ASCII[chr(0xFF21 + i)] = chr(0x41 + i)  # A-Z
    FULLWIDTH_TO_ASCII[chr(0xFF41 + i)] = chr(0x61 + i)  # a-z

# Fullwidth space
FULLWIDTH_TO_ASCII['\u3000'] = ' '

def load_mapping():
    """Load the complete conversion mapping."""
    mapping = dict(CHINESE_TO_ENGLISH_PUNCTUATION)
    mapping.update(FULLWIDTH_TO_ASCII)
    return mapping

def convert_text(text, mapping):
    """
    Convert Chinese punctuation in text to English equivalents.
    
    Args:
        text: Input text string
        mapping: Dictionary mapping Chinese chars to English equivalents
        
    Returns:
        Converted text string
    """
    result = []
    i = 0
    while i < len(text):
        matched = False
        
        # Check for multi-character patterns first
        if i + 1 < len(text):
            two_char = text[i:i+2]
            # Handle special cases like —— (double em dash)
            if two_char == '\u2014\u2014' or two_char == '\uff0d\uff0d':
                result.append('--')
                i += 2
                matched = True
            # Handle ellipsis variations
            elif two_char == '\u2026\u2026' or two_char == '\u22ef\u22ef':
                result.append('......')
                i += 2
                matched = True
        
        if not matched:
            char = text[i]
            if char in mapping:
                result.append(mapping[char])
            else:
                result.append(char)
            i += 1
    
    return ''.join(result)

def convert_file(input_path, output_path=None, encoding='utf-8'):
    """
    Convert Chinese punctuation in a file to English equivalents.
    
    Args:
        input_path: Path to input file
        output_path: Path to output file (if None, overwrite input file)
        encoding: File encoding (default: utf-8)
        
    Returns:
        Tuple of (success: bool, message: str, stats: dict)
    """
    if not os.path.exists(input_path):
        return False, f"Error: File '{input_path}' not found", {}
    
    try:
        # Read input file
        with open(input_path, 'r', encoding=encoding) as f:
            content = f.read()
        
        # Count statistics
        original_length = len(content)
        
        # Load mapping and convert
        mapping = load_mapping()
        converted_content = convert_text(content, mapping)
        
        # Calculate statistics
        changed_count = sum(1 for c in content if c in mapping)
        
        # Write output file
        target_path = output_path if output_path else input_path
        with open(target_path, 'w', encoding=encoding) as f:
            f.write(converted_content)
        
        stats = {
            'original_length': original_length,
            'converted_length': len(converted_content),
            'changed_chars': changed_count,
            'change_percentage': (changed_count / original_length * 100) if original_length > 0 else 0
        }
        
        return True, f"Successfully converted '{os.path.basename(input_path)}'", stats
        
    except UnicodeDecodeError as e:
        return False, f"Error: Encoding issue - {str(e)}. Try specifying a different encoding.", {}
    except Exception as e:
        return False, f"Error: {str(e)}", {}

def main():
    """Main entry point."""
    if len(sys.argv) < 2:
        print(__doc__)
        print("\nExamples:")
        print("  python convert_chinese_punctuation.py input.txt")
        print("  python convert_chinese_punctuation.py input.txt output.txt")
        print("  python convert_chinese_punctuation.py document.txt converted_document.txt")
        sys.exit(1)
    
    input_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else None
    
    print("Converting Chinese punctuation to English...")
    print(f"Input:  {input_file}")
    if output_file:
        print(f"Output: {output_file}")
    else:
        print(f"Output: {input_file} (overwrite)")
    print()
    
    success, message, stats = convert_file(input_file, output_file)
    
    print(message)
    
    if success and stats:
        print(f"\nStatistics:")
        print(f"  Total characters:     {stats['original_length']}")
        print(f"  Changed characters:   {stats['changed_chars']}")
        print(f"  Change percentage:    {stats['change_percentage']:.2f}%")
        print(f"  Output size:          {stats['converted_length']} bytes")
    
    sys.exit(0 if success else 1)

if __name__ == '__main__':
    main()
