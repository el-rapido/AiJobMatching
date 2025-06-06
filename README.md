# CV Job Matcher - Windows Installation Guide

## System Requirements

- **Operating System**: Windows 10/11 (64-bit)
- **RAM**: Minimum 8GB, Recommended 16GB
- **Storage**: At least 5GB free space
- **Internet Connection**: Required for job scraping and API calls

## Prerequisites Installation

### 1. Install Python 3.8+ (Multiple Versions)

**For Maximum Compatibility:**

1. **Keep your existing Python 3.13** for general development
2. **Install Python 3.10.11** for ML/scikit-learn compatibility:
   - Download from [python.org](https://www.python.org/downloads/release/python-31011/)
   - **Important**: Install to `C:\Python310\` (custom location)
   - Check "Add Python to PATH" during installation

3. **Verify both versions**:
   ```cmd
   py -0          # List all Python versions
   py -3.13 --version
   py -3.10 --version
   ```

### 2. Install Node.js and npm

1. Download Node.js LTS from [nodejs.org](https://nodejs.org/)
2. Install with default settings
3. Verify installation:
   ```cmd
   node --version
   npm --version
   ```

### 3. Install CMake

1. **Download CMake** from [cmake.org](https://cmake.org/download/)
   - Choose "Windows x64 Installer" (cmake-3.27.x-windows-x86_64.msi)
2. **During installation**:
   - ✅ Check "Add CMake to system PATH for all users" (or current user)
3. **Verify installation**:
   ```cmd
   cmake --version
   # Should show: cmake version 3.27.x
   ```

### 4. Install Git

1. Download Git from [git-scm.com](https://git-scm.com/download/win)
2. Install with default settings
3. Verify installation:
   ```cmd
   git --version
   ```

### 4. Install Visual Studio Build Tools (for C++ compilation)

**Option A: Visual Studio Community (Recommended)**
1. Download [Visual Studio Community](https://visualstudio.microsoft.com/vs/community/)
2. During installation, select:
   - **Desktop development with C++** workload
   - **MSVC v143 compiler toolset**
   - **Windows 10/11 SDK**

**Option B: Build Tools Only**
1. Download [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022)
2. Select **C++ build tools** workload

### 6. Install vcpkg (C++ Package Manager)

1. Open Command Prompt as Administrator
2. Navigate to C:\ drive:
   ```cmd
   cd C:\
   ```
3. Clone vcpkg:
   ```cmd
   git clone https://github.com/Microsoft/vcpkg.git
   ```
4. Build vcpkg:
   ```cmd
   cd vcpkg
   .\bootstrap-vcpkg.bat
   ```
5. Integrate with Visual Studio:
   ```cmd
   .\vcpkg integrate install
   ```

### 7. Install Required C++ Libraries

```cmd
cd C:\vcpkg
.\vcpkg install curl:x64-windows
.\vcpkg install gumbo:x64-windows
.\vcpkg install nlohmann-json:x64-windows
.\vcpkg install sqlite3:x64-windows
```

### 7. Install Ollama (for AI text processing)

1. Download Ollama from [ollama.ai](https://ollama.ai/download)
2. Install and start Ollama
3. Pull the required model:
   ```cmd
   ollama pull gemma3:4b
   ```

## Project Setup

### 1. Clone the Repository

```cmd
git clone <your-repository-url>
cd cv-job-matcher
```

### 2. Backend Setup (Flask) - Python 3.10 Environment

1. **Create Python 3.10 virtual environment**:
   ```cmd
   cd your-project-directory
   py -3.10 -m venv venv310
   venv310\Scripts\activate
   ```

2. **Install Python dependencies**:
   ```cmd
   pip install --upgrade pip
   pip install flask flask-cors requests python-docx PyPDF2
   pip install scikit-learn==1.3.2 numpy==1.24.4 joblib==1.3.2
   pip install cohere
   ```

3. **Set up environment variables**:
   ```cmd
   # Create .env file in project root
   echo COHERE_API_KEY=your_cohere_api_key_here > .env
   ```
   
   **Get your Cohere API key from**: [dashboard.cohere.ai](https://dashboard.cohere.ai/api-keys)

4. **Always activate the correct environment**:
   ```cmd
   # For ML/Flask development, use Python 3.10:
   venv310\Scripts\activate
   python app.py
   
   # For other tasks, you can still use Python 3.13:
   venv\Scripts\activate
   ```

4. Create required directories:
   ```cmd
   mkdir data output uploads src bin
   ```

### 3. Frontend Setup (React)

1. **Navigate to your project root**:
   ```cmd
   cd C:\Users\Rapido\AiMatching
   ```

2. **Clean any existing installations**:
   
   **PowerShell:**
   ```powershell
   Remove-Item -Recurse -Force node_modules -ErrorAction SilentlyContinue
   Remove-Item package-lock.json -ErrorAction SilentlyContinue
   Remove-Item yarn.lock -ErrorAction SilentlyContinue
   ```
   
   **Command Prompt:**
   ```cmd
   rmdir /s node_modules 2>nul
   del package-lock.json 2>nul
   del yarn.lock 2>nul
   ```

3. **Install React dependencies with specific versions**:
   ```cmd
   npm install react@^18.0.0 react-dom@^18.0.0 react-scripts@5.0.1
   npm install react-router-dom@^6.8.0
   npm install axios@^1.3.0
   ```

4. **Install additional UI dependencies**:
   ```cmd
   npm install @mui/material@^5.11.0 @emotion/react@^11.10.0 @emotion/styled@^11.10.0
   npm install @mui/icons-material@^5.11.0
   npm install react-dropzone@^14.2.0
   npm install styled-components@^5.3.0
   ```

5. **If you're still having issues, try with legacy peer deps**:
   ```cmd
   npm install --legacy-peer-deps
   ```

6. **Check your Node.js version** (should be 16+ for React 18):
   ```cmd
   node --version
   npm --version
   ```

7. **Create or verify package.json scripts**:
   ```json
   {
     "name": "ai-job-matcher-frontend",
     "version": "1.0.0",
     "private": true,
     "dependencies": {
       "react": "^18.0.0",
       "react-dom": "^18.0.0",
       "react-router-dom": "^6.8.0",
       "react-scripts": "5.0.1",
       "axios": "^1.3.0",
       "@mui/material": "^5.11.0",
       "@emotion/react": "^11.10.0",
       "@emotion/styled": "^11.10.0",
       "@mui/icons-material": "^5.11.0",
       "react-dropzone": "^14.2.0",
       "styled-components": "^5.3.0"
     },
     "scripts": {
       "start": "react-scripts start",
       "build": "react-scripts build",
       "test": "react-scripts test",
       "eject": "react-scripts eject"
     },
     "eslintConfig": {
       "extends": [
         "react-app",
         "react-app/jest"
       ]
     },
     "browserslist": {
       "production": [
         ">0.2%",
         "not dead",
         "not op_mini all"
       ],
       "development": [
         "last 1 chrome version",
         "last 1 firefox version",
         "last 1 safari version"
       ]
     }
   }
   ```

### Troubleshooting React Router Issues

If you're still getting `react-router-dom` export errors:

1. **Check if the package is actually installed**:
   ```cmd
   npm list react-router-dom
   ```

2. **Reinstall react-router-dom specifically**:
   ```cmd
   npm uninstall react-router-dom
   npm install react-router-dom@^6.8.0
   ```

3. **Clear npm cache**:
   ```cmd
   npm cache clean --force
   ```

4. **Alternative: Use Yarn instead of npm**:
   ```cmd
   npm install -g yarn
   yarn install
   ```

### 4. Fix CMakeLists.txt Path

Your project already has a CMakeLists.txt file, but you need to update the vcpkg path to match your system:

1. **Edit CMakeLists.txt** and change line 12 from:
   ```cmake
   set(CMAKE_TOOLCHAIN_FILE "C:/Users/preci/vcpkg/scripts/buildsystems/vcpkg.cmake"
   ```
   to:
   ```cmake
   set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
   ```

2. **Create required directory structure**:
   ```cmd
   mkdir src
   mkdir include
   mkdir bin
   
   # Move your source files to the src directory
   move main.cpp src\
   move cv_job_matcher.cpp src\
   move scrapper.cpp src\
   ```

3. **Create the header file** `include/cv_job_matcher.hpp`:
   ```cpp
   #ifndef CV_JOB_MATCHER_HPP
   #define CV_JOB_MATCHER_HPP

   #include <string>

   void match_cv_with_jobs(const std::string& cv_embedding_path, 
                          const std::string& db_path,
                          const std::string& faiss_index_path,
                          int top_k);

   #endif
   ```

### 5. Build the C++ Components

1. **Clean and create build directory**:
   ```cmd
   rmdir /s build 2>nul
   mkdir build
   cd build
   ```

2. **Configure with CMake**:
   ```cmd
   cmake .. -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Release
   ```

3. **Build the project**:
   ```cmd
   cmake --build . --config Release
   ```

4. **Executables will be automatically copied to the bin directory** (thanks to your CMakeLists.txt configuration)

## Configuration

### 1. Update File Paths

Ensure all Python scripts can find each other by updating the paths in `app.py`:

```python
# In app.py, verify these paths match your structure
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
UPLOAD_FOLDER = os.path.join(BASE_DIR, 'uploads')
DATA_DIR = os.path.join(BASE_DIR, 'data')
OUTPUT_DIR = os.path.join(BASE_DIR, 'output')
```

### 2. Test Individual Components

1. **Test CV Processor**:
   ```cmd
   python src\cv_processor.py sample_cv.pdf data\processed_cv.txt
   ```

2. **Test Embedder**:
   ```cmd
   python src\embedder.py --file data\sample_cv.txt --output output\embedding.json
   ```

3. **Test Job Scraper**:
   ```cmd
   bin\job_scraper.exe --job-title "Software Developer" --location "Remote" --max-jobs 5
   ```

## Running the Application

### 1. Start the Backend

```cmd
# Activate virtual environment
venv\Scripts\activate

# Start Flask server
python app.py
```

The backend should start on `http://localhost:5000`

### 2. Start the Frontend

In a new terminal:
```cmd
cd frontend
npm start
```

The frontend should start on `http://localhost:3000`

### 3. Using the CLI Version

For command-line usage:
```cmd
bin\ai_job_matcher.exe --cv-file your_cv.pdf --job-title "Software Developer" --location "Remote" --scrape
```

## Troubleshooting

### Common Issues

**1. Python module not found**
```cmd
# Ensure virtual environment is activated
venv\Scripts\activate
pip install <missing-module>
```

**2. C++ compilation errors**
```cmd
# Verify vcpkg integration
C:\vcpkg\vcpkg integrate install
# Rebuild with verbose output
cmake --build . --config Release --verbose
```

**3. Ollama not responding**
```cmd
# Restart Ollama service
ollama serve
# In another terminal, test:
ollama list
```

**4. API key issues**
- Verify your Cohere API key is valid
- Check the `.env` file is in the correct location
- Ensure the key has sufficient credits

**5. Job scraper timeout**
- Reduce `--max-jobs` parameter
- Check internet connection
- Some job sites may block requests

### Performance Tips

1. **For faster job scraping**: Use `--max-jobs 10` instead of higher numbers
2. **For better matching**: Ensure your CV has clear sections (skills, experience, education)
3. **Memory usage**: Close other applications when processing large CV files

## File Structure

# AiMatching Project Structure

```
AiMatching/                     # Your project root
├── CMakeLists.txt             # Your existing CMake configuration
├── app.py                     # Flask backend
├── src/                       # Source files directory
│   ├── main.cpp               # CLI main application
│   ├── cv_job_matcher.cpp     # CV matching logic
│   ├── scrapper.cpp           # Job scraping logic
│   ├── cv_processor.py        # CV text extraction
│   ├── embedder.py            # Text embedding generation
│   └── job_matcher.py         # Job matching algorithm
├── include/                   # Header files
│   └── cv_job_matcher.hpp
├── components/                # React components
├── pages/                     # React pages
├── services/                  # Frontend services
├── data/                      # Data storage
├── output/                    # Generated files
├── uploads/                   # Uploaded CV files
├── bin/                       # Compiled executables (auto-generated)
└── build/                     # CMake build files
```

## Next Steps

1. **Upload a CV** via the web interface at `http://localhost:3000`
2. **Configure job search** parameters (title, location)
3. **Review matches** and similarity scores
4. **Export results** as needed

For advanced usage and API documentation, refer to the individual script help:
```cmd
python src\embedder.py --help
bin\ai_job_matcher.exe --help
```
