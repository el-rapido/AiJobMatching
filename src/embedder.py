import os
import requests
import json
import argparse
import subprocess
import sqlite3
from typing import List, Union, Dict, Any


class Embedder:
    """A class that handles text embedding using the Cohere API."""

    def __init__(self, api_key: str = None):
        """
        Initialize the Embedder with an API key.
        
        Args:
            api_key: Cohere API key. If None, will try to get from environment variables.
        """
        if api_key is None:
            api_key = os.environ.get("COHERE_API_KEY")
            if api_key is None:
                raise ValueError("API key not provided and COHERE_API_KEY environment variable not set")
        
        self.api_key = api_key
        self.endpoint = "https://api.cohere.ai/v1/embed"
        print(f"[Embedder] Initialized with API key: {api_key[:5]}...")
    
    def generate_embedding(self, text: str) -> List[float]:
        """
        Generate an embedding for the provided text.
        
        Args:
            text: The text to generate embedding for
            
        Returns:
            A list of floats representing the embedding vector
        """
        print(f"[Embedder] Generating embedding for text ({len(text)} characters)...")
        
        # Prepare the payload for Cohere API
        payload = {
            "texts": [text],
            "model": "embed-english-v3.0",
            "input_type": "search_document"
        }
        
        # Set up headers
        headers = {
            "Authorization": f"Bearer {self.api_key}",
            "Content-Type": "application/json",
            "Accept": "application/json"
        }
        
        # Make the API request
        try:
            print("[Embedder] Making API request...")
            response = requests.post(
                self.endpoint,
                headers=headers,
                json=payload,
                timeout=30
            )
            
            print(f"[Embedder] API response status code: {response.status_code}")
            
            # Check if request was successful
            if response.status_code != 200:
                error_message = f"API request failed with status {response.status_code}: {response.text}"
                print(f"[Embedder] Error: {error_message}")
                raise Exception(error_message)
            
            # Parse the response
            response_data = response.json()
            
            # Extract embedding from response
            if "embeddings" in response_data and len(response_data["embeddings"]) > 0:
                embedding = response_data["embeddings"][0]
                print(f"[Embedder] Successfully generated embedding with {len(embedding)} dimensions")
                return embedding
            else:
                # Try alternative format
                print("[Embedder] 'embeddings' field not found, trying alternative format")
                if "embedding" in response_data:
                    embedding = response_data["embedding"]
                    print(f"[Embedder] Successfully generated embedding with {len(embedding)} dimensions")
                    return embedding
                
                # If we still can't find the embedding
                print(f"[Embedder] Available keys in response: {list(response_data.keys())}")
                raise Exception("Embedding field not found in response")
                
        except Exception as e:
            print(f"[Embedder] Exception in generate_embedding: {str(e)}")
            raise
    
    def generate_embeddings_batch(self, texts: List[str]) -> List[List[float]]:
        """
        Generate embeddings for multiple texts in a single API call.
        
        Args:
            texts: List of texts to generate embeddings for
            
        Returns:
            List of embedding vectors
        """
        print(f"[Embedder] Generating embeddings for {len(texts)} texts in batch...")
        
        # Prepare the payload for Cohere API
        payload = {
            "texts": texts,
            "model": "embed-english-v3.0",
            "input_type": "search_document"
        }
        
        # Set up headers
        headers = {
            "Authorization": f"Bearer {self.api_key}",
            "Content-Type": "application/json",
            "Accept": "application/json"
        }
        
        # Make the API request
        try:
            response = requests.post(
                self.endpoint,
                headers=headers,
                json=payload,
                timeout=240
            )
            
            # Check if request was successful
            if response.status_code != 200:
                raise Exception(f"API request failed with status {response.status_code}: {response.text}")
            
            # Parse the response
            response_data = response.json()
            
            # Extract embeddings from response
            if "embeddings" in response_data:
                embeddings = response_data["embeddings"]
                print(f"[Embedder] Successfully generated {len(embeddings)} embeddings")
                return embeddings
            else:
                # Try alternative format
                print("[Embedder] 'embeddings' field not found, trying alternative format")
                if "embedding" in response_data:
                    embeddings = [response_data["embedding"]]
                    print(f"[Embedder] Successfully generated {len(embeddings)} embeddings")
                    return embeddings
                
                # If we still can't find the embeddings
                print(f"[Embedder] Available keys in response: {list(response_data.keys())}")
                raise Exception("Embedding field not found in response")
                
        except Exception as e:
            print(f"[Embedder] Exception in generate_embeddings_batch: {str(e)}")
            raise


def load_file_text(file_path: str) -> str:
    """
    Load text content from a file.
    
    Args:
        file_path: Path to the file
        
    Returns:
        Text content of the file
    """
    try:
        print(f"[Embedder] Attempting to load file from: {file_path}")
        
        with open(file_path, 'r', encoding='utf-8') as f:
            content = f.read()
        print(f"[Embedder] Successfully loaded file content from: {file_path}")
        return content
    except Exception as e:
        print(f"[Embedder] Failed to open file {file_path}: {str(e)}")
        print(f"[Embedder] Current working directory: {os.getcwd()}")
        raise RuntimeError(f"Failed to open file {file_path}: {str(e)}")


def save_embedding(embedding: List[float], output_path: str) -> None:
    """
    Save an embedding to a file.
    
    Args:
        embedding: The embedding vector to save
        output_path: Path where to save the embedding
    """
    try:
        # Ensure the directory exists
        output_dir = os.path.dirname(output_path)
        if output_dir:
            os.makedirs(output_dir, exist_ok=True)
            print(f"[Embedder] Ensuring directory exists: {output_dir}")
        
        print(f"[Embedder] Saving embedding to: {output_path}")
        
        with open(output_path, 'w') as f:
            json.dump(embedding, f)
        print(f"[Embedder] Embedding saved successfully to: {output_path}")
    except Exception as e:
        print(f"[Embedder] Error saving embedding: {str(e)}")
        print(f"[Embedder] Current working directory: {os.getcwd()}")
        raise


def filter_cv_with_ollama(raw_text: str, model: str = "gemma3:4b", output_dir: str = None) -> str:
    """
    Filter CV text using Ollama with Gemma model.
    
    Args:
        raw_text: Raw CV text to filter
        model: Ollama model to use for filtering
        output_dir: Directory to save filtered text output
        
    Returns:
        Filtered CV text
    """
    prompt = (
        "You are a professional CV parser. Extract and structure the following CV data for job matching:\n\n"
        "1. Professional Summary\n"
        "2. Work Experience (with correct company names, locations, dates)\n"
        "3. Education (with correct institution names)\n"
        "4. Skills (technical and soft skills)\n"
        "5. Languages\n"
        "6. Projects\n"
        "7. Certifications\n\n"
        "Format the output clearly with section headers. Keep all location names and dates exactly as they appear.\n"
        "Omit personal interests, hobbies, references, and irrelevant details.\n"
        "Return ONLY the structured CV data in txt format without ANY explanations, asterisks or additional commentary AT ALL.\n\n"
        f"CV Text:\n{raw_text}"
    )

    try:
        print("[Filter] Calling Ollama locally with Gemma...")
        print(f"[Filter] Using model: {model}")
        
        # Use the encoding parameter with UTF-8 and errors='ignore' to handle problematic characters
        result = subprocess.run(
            ["ollama", "run", model],
            input=prompt.encode('utf-8'),  # Encode input as UTF-8
            capture_output=True,
            timeout=240
        )

        # Decode output with error handling
        filtered_text = result.stdout.decode('utf-8', errors='ignore').strip()
        
        print(f"[Filter] Filtering complete. Output length: {len(filtered_text)} characters.")
        
        # Print debug preview of Gemma's output (first 200 chars)
        print(f"[Filter] DEBUG - Gemma output preview (first 200 chars):")
        print("-" * 50)
        print(filtered_text[:200] + ("..." if len(filtered_text) > 200 else ""))
        print("-" * 50)
        
        # Save filtered text to file if output directory is provided
        if output_dir:
            try:
                # Create output directory if it doesn't exist
                os.makedirs(output_dir, exist_ok=True)
                
                # Generate timestamped filename
                import datetime
                timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
                output_file = os.path.join(output_dir, f"filtered_cv_{timestamp}.txt")
                
                # Save filtered text to file
                with open(output_file, 'w', encoding='utf-8') as f:
                    f.write(filtered_text)
                
                print(f"[Filter] Saved filtered CV text to: {output_file}")
            except Exception as e:
                print(f"[Filter] Warning: Could not save filtered text to file: {str(e)}")
        
        return filtered_text

    except subprocess.CalledProcessError as e:
        print(f"[Filter] Ollama command failed: {e}")
        print(f"[Filter] Stderr: {e.stderr.decode('utf-8', errors='ignore')}")
        raise RuntimeError(f"Ollama filtering failed: {e}")
    except Exception as e:
        print(f"[Filter] Error calling Ollama: {str(e)}")
        # Fall back to using the original text if Ollama filtering fails
        print(f"[Filter] Falling back to using original text without filtering")
        return raw_text


def load_job_json(file_path: str) -> Dict[str, Any]:
    """
    Load job details from a JSON file.
    
    Args:
        file_path: Path to the JSON file containing job details
        
    Returns:
        Dictionary containing job details
    """
    try:
        print(f"[Embedder] Loading job details from JSON file: {file_path}")
        
        with open(file_path, 'r', encoding='utf-8') as f:
            job_data = json.load(f)
        
        print(f"[Embedder] Successfully loaded job details from: {file_path}")
        return job_data
    except json.JSONDecodeError as e:
        print(f"[Embedder] Invalid JSON format in file {file_path}: {str(e)}")
        raise RuntimeError(f"Invalid JSON format in file {file_path}: {str(e)}")
    except Exception as e:
        print(f"[Embedder] Failed to load job JSON from {file_path}: {str(e)}")
        print(f"[Embedder] Current working directory: {os.getcwd()}")
        raise RuntimeError(f"Failed to load job JSON from {file_path}: {str(e)}")


def format_job_for_embedding(job_data: Dict[str, Any]) -> str:
    """
    Format job data into a string suitable for embedding.
    
    Args:
        job_data: Dictionary containing job details
        
    Returns:
        Formatted string representation of the job
    """
    # Extract job details with fallbacks for missing fields
    title = job_data.get("title", "")
    description = job_data.get("description", "")
    skills = job_data.get("skills", [])
    location = job_data.get("location", "")
    source = job_data.get("source", "")
    
    # Format skills as a comma-separated string
    skills_str = ", ".join(skills) if skills else ""
    
    # Build formatted text for embedding
    formatted_text = f"Job Title: {title}\n\n"
    formatted_text += f"Description: {description}\n\n"
    
    if skills_str:
        formatted_text += f"Required Skills: {skills_str}\n\n"
    
    if location:
        formatted_text += f"Location: {location}\n\n"
    
    if source:
        formatted_text += f"Source: {source}"
    
    print(f"[Embedder] Formatted job data for embedding ({len(formatted_text)} characters)")
    return formatted_text


def save_job_to_database(job_data: Dict[str, Any], embedding: List[float], db_path: str) -> None:
    """
    Save job details along with its embedding to a SQLite database.
    
    Args:
        job_data: Dictionary containing job details
        embedding: The embedding vector for the job
        db_path: Path to SQLite database
    """
    try:
        # Ensure the directory exists
        db_dir = os.path.dirname(db_path)
        if db_dir:
            os.makedirs(db_dir, exist_ok=True)
            print(f"[Embedder] Ensuring database directory exists: {db_dir}")
        
        print(f"[Embedder] Saving job with embedding to database: {db_path}")
        
        # Extract job details
        title = job_data.get("title", "")
        description = job_data.get("description", "")
        location = job_data.get("location", "")
        source = job_data.get("source", "")
        
        # Handle skills array
        skills = job_data.get("skills", [])
        skills_str = json.dumps(skills) if skills else "[]"
        
        # Convert embedding to JSON string
        embedding_json = json.dumps(embedding)
        
        # Connect to database
        conn = sqlite3.connect(db_path)
        cursor = conn.cursor()
        
        # Create tables if they don't exist
        cursor.execute('''
        CREATE TABLE IF NOT EXISTS jobs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT,
            description TEXT,
            location TEXT,
            source TEXT,
            skills TEXT,
            embedding TEXT,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
        ''')
        
        # Insert job with embedding
        cursor.execute(
            '''
            INSERT INTO jobs (title, description, location, source, skills, embedding)
            VALUES (?, ?, ?, ?, ?, ?)
            ''',
            (title, description, location, source, skills_str, embedding_json)
        )
        
        # Commit changes and close connection
        conn.commit()
        job_id = cursor.lastrowid
        conn.close()
        
        print(f"[Embedder] Job with embedding saved successfully to database with ID: {job_id}")
        
    except sqlite3.Error as e:
        print(f"[Embedder] SQLite error: {str(e)}")
        raise
    except Exception as e:
        print(f"[Embedder] Error saving job to database: {str(e)}")
        raise


def main():
    """
    Main function to handle command-line embedding generation.
    """
    parser = argparse.ArgumentParser(description="Generate text embeddings using Cohere API")
    parser.add_argument("--text", type=str, help="Text to generate embedding for")
    parser.add_argument("--file", type=str, help="File containing text to generate embedding for")
    parser.add_argument("--output", type=str, help="Output file path")
    parser.add_argument("--api-key", type=str, help="Cohere API key (will use COHERE_API_KEY env var if not provided)")
    parser.add_argument("--skip-filter", action="store_true", help="Skip Ollama filtering step")
    parser.add_argument("--save-filtered", action="store_true", help="Save filtered CV text to output directory")
    
    # New arguments for job processing
    parser.add_argument("--job-file", type=str, help="JSON file containing job details")
    parser.add_argument("--job-dir", type=str, help="Directory containing multiple job JSON files")
    parser.add_argument("--db-path", type=str, help="Path to SQLite database for storing jobs with embeddings")
    
    args = parser.parse_args()
    
    # Use the API key from args or fallback to hardcoded value
    api_key = args.api_key or ""
    
    try:
        # Determine the directory structure based on current script location
        script_dir = os.path.dirname(os.path.abspath(__file__))
        project_root = os.path.dirname(script_dir)
        
        # Define default paths relative to project root
        default_input_file = os.path.join(project_root, "data", "sample_cv.txt")
        default_output_file = os.path.join(project_root, "output", "embedding.json")
        filtered_output_dir = os.path.join(project_root, "output", "filtered")
        
        # Define default paths for job processing
        default_job_dir = os.path.join(project_root, "data", "jobs")
        default_db_path = os.path.join(project_root, "data", "jobs.db")
        
        print(f"[Embedder] Script directory: {script_dir}")
        print(f"[Embedder] Project root: {project_root}")
        
        embedder = Embedder(api_key)
        
        # Check if we're processing a CV
        if args.text or args.file or (not args.job_file and not args.job_dir):
            print("[Embedder] Processing CV mode...")
            print(f"[Embedder] Default CV input file: {default_input_file}")
            print(f"[Embedder] Default CV output file: {default_output_file}")
            if args.save_filtered:
                print(f"[Embedder] Filtered output directory: {filtered_output_dir}")
            
            # Get input text
            if args.text:
                text = args.text
            elif args.file:
                # Use the provided file path
                file_path = args.file
                # First load the raw text
                raw_text = load_file_text(file_path)
            else:
                # Use the default file path
                print(f"[Embedder] No input file specified, using default: {default_input_file}")
                raw_text = load_file_text(default_input_file)
            
            # Filter CV with Ollama if we loaded from a file and filtering is not skipped
            if 'raw_text' in locals() and not args.skip_filter:
                try:
                    # Set output directory for filtered text if save-filtered flag is set
                    output_dir = filtered_output_dir if args.save_filtered else None
                    text = filter_cv_with_ollama(raw_text, output_dir=output_dir)
                except Exception as e:
                    print(f"[Embedder] Filtering failed, using original text: {str(e)}")
                    text = raw_text
                    
                    # Still save the original text if save-filtered flag is set
                    if args.save_filtered:
                        try:
                            import datetime
                            os.makedirs(filtered_output_dir, exist_ok=True)
                            timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
                            original_file = os.path.join(filtered_output_dir, f"original_cv_{timestamp}.txt")
                            with open(original_file, 'w', encoding='utf-8') as f:
                                f.write(raw_text)
                            print(f"[Embedder] Saved original CV text to: {original_file}")
                        except Exception as save_error:
                            print(f"[Embedder] Warning: Could not save original text to file: {str(save_error)}")
            elif 'raw_text' in locals() and args.skip_filter:
                print("[Embedder] Skipping filtering step as requested")
                text = raw_text
                
                # Save the original text if save-filtered flag is set
                if args.save_filtered:
                    try:
                        import datetime
                        os.makedirs(filtered_output_dir, exist_ok=True)
                        timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
                        original_file = os.path.join(filtered_output_dir, f"original_cv_{timestamp}.txt")
                        with open(original_file, 'w', encoding='utf-8') as f:
                            f.write(raw_text)
                        print(f"[Embedder] Saved original CV text to: {original_file}")
                    except Exception as e:
                        print(f"[Embedder] Warning: Could not save original text to file: {str(e)}")
            
            # Generate embedding
            embedding = embedder.generate_embedding(text)
            
            # Print some info about the embedding
            print(f"\n[Embedder] Embedding generated successfully!")
            print(f"Embedding size: {len(embedding)} dimensions")
            print(f"First 10 dimensions: {', '.join(map(str, embedding[:10]))}")
            
            # Determine output file path
            output_path = args.output if args.output else default_output_file
            
            # Save embedding
            save_embedding(embedding, output_path)
        
        # Check if we're processing a single job file
        if args.job_file:
            process_single_job(args, embedder, default_db_path)
        
        # Check if we're processing a directory of job files
        if args.job_dir:
            process_job_directory(args, embedder, default_job_dir, default_db_path)
            
    except Exception as e:
        print(f"\n[ERROR] {str(e)}")
        exit(1)


def process_single_job(args, embedder, default_db_path):
    """
    Process a single job file, generate embedding, and save to database.
    
    Args:
        args: Command-line arguments
        embedder: Embedder instance
        default_db_path: Default database path for job embeddings
    """
    print(f"[Embedder] Processing single job file: {args.job_file}")
    
    # Load job data from file
    job_data = load_job_json(args.job_file)
    
    # Check if job_data is a list (multiple jobs in one file)
    if isinstance(job_data, list):
        print(f"[Embedder] Found {len(job_data)} jobs in file")
        
        # Process each job in the list
        for job in job_data:
            # Format job data for embedding
            formatted_job = format_job_for_embedding(job)
            
            # Generate embedding
            embedding = embedder.generate_embedding(formatted_job)
            
            # Print some info about the embedding
            print(f"\n[Embedder] Job embedding generated successfully!")
            print(f"Embedding size: {len(embedding)} dimensions")
            print(f"First 10 dimensions: {', '.join(map(str, embedding[:10]))}")
            
            # Determine database path
            db_path = args.db_path if args.db_path else default_db_path
            
            # Save job with embedding to database
            save_job_to_database(job, embedding, db_path)
    else:
        # Process a single job object
        # Format job data for embedding
        formatted_job = format_job_for_embedding(job_data)
        
        # Generate embedding
        embedding = embedder.generate_embedding(formatted_job)
        
        # Print some info about the embedding
        print(f"\n[Embedder] Job embedding generated successfully!")
        print(f"Embedding size: {len(embedding)} dimensions")
        print(f"First 10 dimensions: {', '.join(map(str, embedding[:10]))}")
        
        # Determine database path
        db_path = args.db_path if args.db_path else default_db_path
        
        # Save job with embedding to database
        save_job_to_database(job_data, embedding, db_path)


def process_job_directory(args, embedder, default_job_dir, default_db_path):
    """
    Process all job files in a directory, generate embeddings, and save to database.
    
    Args:
        args: Command-line arguments
        embedder: Embedder instance
        default_job_dir: Default directory containing job files
        default_db_path: Default database path for job embeddings
    """
    # Determine job directory
    job_dir = args.job_dir if args.job_dir else default_job_dir
    print(f"[Embedder] Processing jobs from directory: {job_dir}")
    
    # Determine database path
    db_path = args.db_path if args.db_path else default_db_path
    print(f"[Embedder] Database path for processed jobs: {db_path}")
    
    # Get list of JSON files in the job directory
    try:
        job_files = [f for f in os.listdir(job_dir) if f.endswith('.json')]
        print(f"[Embedder] Found {len(job_files)} job files in directory")
        
        if not job_files:
            print(f"[Embedder] No job files found in directory: {job_dir}")
            return
        
        # Process each job file
        for job_file in job_files:
            try:
                # Full path to job file
                job_file_path = os.path.join(job_dir, job_file)
                print(f"\n[Embedder] Processing job file: {job_file}")
                
                # Load job data from file
                job_data = load_job_json(job_file_path)
                
                # Format job data for embedding
                formatted_job = format_job_for_embedding(job_data)
                
                # Generate embedding
                embedding = embedder.generate_embedding(formatted_job)
                
                # Save job with embedding to database
                save_job_to_database(job_data, embedding, db_path)
                
            except Exception as e:
                print(f"[Embedder] Error processing job file {job_file}: {str(e)}")
                print("[Embedder] Continuing with next file...")
                continue
        
        print(f"\n[Embedder] Completed processing {len(job_files)} job files")
        
    except Exception as e:
        print(f"[Embedder] Error accessing job directory {job_dir}: {str(e)}")
        raise


if __name__ == "__main__":
    main()