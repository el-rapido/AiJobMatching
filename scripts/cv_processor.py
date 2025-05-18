import os
import sys
from docx import Document
import PyPDF2

def docx_to_text(docx_path):
    try:
        doc = Document(docx_path)
        return '\n'.join([para.text for para in doc.paragraphs])
    except Exception as e:
        raise RuntimeError(f"Error reading .docx file: {e}")

def pdf_to_text(pdf_path):
    try:
        with open(pdf_path, 'rb') as file:
            reader = PyPDF2.PdfReader(file)
            text = ''
            for page in reader.pages:
                text += page.extract_text() or ''
            return text
    except Exception as e:
        raise RuntimeError(f"Error reading .pdf file: {e}")

def save_text_to_file(text, output_path):
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(text)

def convert_cv(input_path, output_path='data/processed_cv.txt'):
    ext = os.path.splitext(input_path)[1].lower()
    if ext == '.docx':
        text = docx_to_text(input_path)
    elif ext == '.pdf':
        text = pdf_to_text(input_path)
    else:
        raise ValueError("Unsupported file format. Use .docx or .pdf")
    
    save_text_to_file(text, output_path)
    print(f"✅ CV text saved to {output_path}")

# CLI Usage
if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python cv_preprocessor.py <cv_path>")
    else:
        convert_cv(sys.argv[1])
