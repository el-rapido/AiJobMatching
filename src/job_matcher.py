import os
import json
import argparse
import sqlite3
import numpy as np
import faiss
from typing import List, Dict, Any, Tuple, Optional
from sklearn.preprocessing import normalize


def load_cv_embedding(embedding_path: str) -> np.ndarray:
    """
    Load CV embedding from a JSON file.
    
    Args:
        embedding_path: Path to the JSON file containing the CV embedding
        
    Returns:
        NumPy array of the embedding vector
    """
    try:
        print(f"[JobMatcher] Loading CV embedding from: {embedding_path}")
        
        with open(embedding_path, 'r', encoding='utf-8') as f:
            embedding = json.load(f)
        
        # Convert to numpy array
        embedding_array = np.array(embedding, dtype=np.float32).reshape(1, -1)
        
        print(f"[JobMatcher] Successfully loaded CV embedding with shape: {embedding_array.shape}")
        return embedding_array
    except Exception as e:
        print(f"[JobMatcher] Error loading CV embedding: {str(e)}")
        raise


def load_jobs_from_db(db_path: str) -> Tuple[np.ndarray, List[Dict[str, Any]]]:
    """
    Load jobs and their embeddings from the SQLite database.
    
    Args:
        db_path: Path to the SQLite database
        
    Returns:
        Tuple containing:
            - NumPy array of job embeddings
            - List of job dictionaries with metadata
    """
    try:
        print(f"[JobMatcher] Loading jobs from database: {db_path}")
        
        # Connect to database
        conn = sqlite3.connect(db_path)
        conn.row_factory = sqlite3.Row  # This enables column access by name
        cursor = conn.cursor()
        
        # Query jobs table
        cursor.execute("SELECT id, title, description, location, source, skills, embedding FROM jobs")
        rows = cursor.fetchall()
        
        if not rows:
            print(f"[JobMatcher] No jobs found in database")
            conn.close()
            return np.array([], dtype=np.float32), []
        
        # Process results
        job_embeddings = []
        job_metadata = []
        
        for row in rows:
            # Parse embedding from JSON string
            embedding = json.loads(row['embedding'])
            job_embeddings.append(embedding)
            
            # Parse skills from JSON string
            skills = json.loads(row['skills']) if row['skills'] else []
            
            # Create job metadata dictionary
            job_metadata.append({
                'id': row['id'],
                'title': row['title'],
                'description': row['description'],
                'location': row['location'],
                'source': row['source'],
                'skills': skills
            })
        
        # Convert to numpy array
        embeddings_array = np.array(job_embeddings, dtype=np.float32)
        
        print(f"[JobMatcher] Successfully loaded {len(job_metadata)} jobs")
        print(f"[JobMatcher] Embeddings array shape: {embeddings_array.shape}")
        
        conn.close()
        return embeddings_array, job_metadata
    
    except sqlite3.Error as e:
        print(f"[JobMatcher] SQLite error: {str(e)}")
        raise
    except Exception as e:
        print(f"[JobMatcher] Error loading jobs from database: {str(e)}")
        raise


def extract_cv_key_info(cv_text_path: Optional[str] = None) -> Dict[str, Any]:
    """
    Extract key information from the CV text to enhance matching.
    
    Args:
        cv_text_path: Path to the CV text file (optional)
        
    Returns:
        Dictionary with key CV information
    """
    try:
        cv_info = {
            'experience_years': 0,
            'education_level': '',
            'key_skills': [],
            'job_titles': [],
            'industries': []
        }
        
        if not cv_text_path:
            return cv_info
            
        print(f"[JobMatcher] Extracting key info from CV: {cv_text_path}")
        
        with open(cv_text_path, 'r', encoding='utf-8') as f:
            cv_text = f.read()
        
        # Simple extraction logic - in a production system, you'd use NLP techniques
        # Extract skills (simple approach)
        sections = cv_text.split("\n\n")
        skills_section = ""
        for section in sections:
            if "SKILLS" in section:
                skills_section = section
                break
        
        if skills_section:
            lines = skills_section.split("\n")
            for line in lines:
                if "*" in line:
                    skill = line.split("*")[1].strip()
                    if skill:
                        cv_info['key_skills'].append(skill)
        
        # Extract job titles
        experience_section = ""
        for section in sections:
            if "EXPERIENCE" in section:
                experience_section = section
                break
                
        if experience_section:
            lines = experience_section.split("\n")
            for i, line in enumerate(lines):
                if "(" in line and ")" in line:
                    parts = line.split("(")
                    if len(parts) > 1:
                        job_title = parts[1].split(")")[0].strip()
                        cv_info['job_titles'].append(job_title)
        
        # Extract education level
        education_section = ""
        for section in sections:
            if "EDUCATION" in section:
                education_section = section
                break
                
        if education_section:
            lines = education_section.split("\n")
            for line in lines:
                if "Diploma" in line:
                    cv_info['education_level'] = line.strip()
                    break
            
        print(f"[JobMatcher] Extracted CV info: {len(cv_info['key_skills'])} skills, {len(cv_info['job_titles'])} job titles")
        return cv_info
        
    except Exception as e:
        print(f"[JobMatcher] Error extracting CV info: {str(e)}")
        return cv_info


def calculate_job_relevance_scores(
    job_metadata: List[Dict[str, Any]], 
    cv_info: Dict[str, Any]
) -> List[float]:
    """
    Calculate relevance scores based on keyword matching to supplement embedding similarity.
    
    Args:
        job_metadata: List of job dictionaries with metadata
        cv_info: Dictionary with key CV information
        
    Returns:
        List of relevance scores
    """
    try:
        relevance_scores = []
        
        for job in job_metadata:
            score = 0.0
            
            # Check for skill matches (weighted heavily)
            job_skills = [skill.lower() for skill in job['skills']]
            cv_skills = [skill.lower() for skill in cv_info['key_skills']]
            
            for skill in cv_skills:
                if any(skill in job_skill for job_skill in job_skills):
                    score += 0.2  # High weight for skill matches
            
            # Check for job title matches
            job_title = job['title'].lower()
            for title in cv_info['job_titles']:
                title = title.lower()
                if title in job_title or job_title in title:
                    score += 0.3  # High weight for job title matches
            
            # Normalize score between 0 and 1
            score = min(score, 1.0)
            relevance_scores.append(score)
            
        return relevance_scores
        
    except Exception as e:
        print(f"[JobMatcher] Error calculating relevance scores: {str(e)}")
        return [0.0] * len(job_metadata)


def find_matching_jobs(
    cv_embedding: np.ndarray, 
    job_embeddings: np.ndarray, 
    job_metadata: List[Dict[str, Any]], 
    top_k: int,
    cv_text_path: Optional[str] = None,
    min_similarity: float = 0.4
) -> List[Dict[str, Any]]:
    """
    Find the most similar jobs to a CV using a hybrid approach of FAISS and keyword matching.
    
    Args:
        cv_embedding: NumPy array of the CV embedding
        job_embeddings: NumPy array of job embeddings
        job_metadata: List of job dictionaries with metadata
        top_k: Number of top matches to return
        cv_text_path: Path to the CV text file (optional)
        min_similarity: Minimum similarity threshold
        
    Returns:
        List of job dictionaries with similarity scores
    """
    try:
        print(f"[JobMatcher] Finding top {top_k} matching jobs...")
        
        # Check if we have any job embeddings
        if job_embeddings.size == 0:
            print("[JobMatcher] No job embeddings available for matching")
            return []
        
        # Get dimension from job embeddings
        dimension = job_embeddings.shape[1]
        
        # Create FAISS index
        print(f"[JobMatcher] Creating FAISS index with dimension {dimension}")
        index = faiss.IndexFlatIP(dimension)  # Inner product (cosine similarity)
        
        # Normalize vectors for cosine similarity
        # Using sklearn normalize instead of faiss normalize to ensure consistent normalization
        cv_embedding_normalized = normalize(cv_embedding, norm='l2')
        job_embeddings_normalized = normalize(job_embeddings, norm='l2')
        
        # Add job embeddings to index
        index.add(job_embeddings_normalized)
        
        # Search for matches - get more candidates than needed since we'll filter later
        candidates_k = min(top_k * 3, len(job_metadata))
        distances, indices = index.search(cv_embedding_normalized, candidates_k)
        
        # Extract CV key information
        cv_info = extract_cv_key_info(cv_text_path)
        
        # Prepare candidates for hybrid scoring
        candidates = []
        candidate_metadata = []
        
        for i in range(len(indices[0])):
            idx = indices[0][i]
            similarity = float(distances[0][i])  # Convert numpy float to Python float
            
            # Skip invalid indices
            if idx < 0 or idx >= len(job_metadata):
                continue
                
            # Skip jobs with low similarity
            if similarity < min_similarity:
                continue
            
            candidates.append(job_metadata[idx])
            candidate_metadata.append({'index': idx, 'embedding_similarity': similarity})
        
        # Calculate relevance scores based on keyword matching
        relevance_scores = calculate_job_relevance_scores(candidates, cv_info)
        
        # Combine embedding similarity and relevance scores
        combined_scores = []
        for i in range(len(candidates)):
            # Weight: 60% embedding similarity, 40% keyword relevance
            combined_score = 0.6 * candidate_metadata[i]['embedding_similarity'] + 0.4 * relevance_scores[i]
            combined_scores.append(combined_score)
        
        # Sort candidates by combined score
        sorted_indices = np.argsort(combined_scores)[::-1]  # Descending order
        
        # Prepare final results
        matches = []
        
        for i in sorted_indices[:top_k]:
            job = candidates[i].copy()  # Make a copy to avoid modifying the original
            
            # Add scores
            job['similarity'] = float(combined_scores[i])  # Combined score
            job['embedding_similarity'] = float(candidate_metadata[i]['embedding_similarity'])
            job['keyword_relevance'] = float(relevance_scores[i])
            
            # Only include if combined score meets threshold
            if job['similarity'] >= min_similarity:
                matches.append(job)
        
        if not matches:
            print("[JobMatcher] No jobs met the minimum similarity threshold")
            return []
            
        print(f"[JobMatcher] Found {len(matches)} matching jobs")
        return matches
    
    except Exception as e:
        print(f"[JobMatcher] Error finding matching jobs: {str(e)}")
        raise


def format_job_description_preview(description: str, max_length: int = 100) -> str:
    """
    Format a job description preview.
    
    Args:
        description: Full job description
        max_length: Maximum length of the preview
        
    Returns:
        Formatted job description preview
    """
    if not description:
        return "No description available."
    
    # Get first sentence or paragraph
    sentences = description.split('.')
    first_sentence = sentences[0].strip()
    
    # Truncate if too long
    if len(first_sentence) > max_length:
        first_sentence = first_sentence[:max_length] + "..."
    
    return first_sentence


def save_matches_to_file(matches: List[Dict[str, Any]], output_path: str, format_type: str = "json") -> None:
    """
    Save matching jobs to a file in JSON or text format.
    
    Args:
        matches: List of job dictionaries with similarity scores
        output_path: Path to save the output
        format_type: Output format type ("json" or "txt")
    """
    try:
        print(f"[JobMatcher] Saving matches to: {output_path}")
        
        # Ensure the directory exists
        output_dir = os.path.dirname(output_path)
        if output_dir:
            os.makedirs(output_dir, exist_ok=True)
        
        if format_type.lower() == "json":
            # Save matches to JSON
            with open(output_path, 'w', encoding='utf-8') as f:
                json.dump(matches, f, indent=2)
        else:
            # Save matches to text file
            with open(output_path, 'w', encoding='utf-8') as f:
                f.write("============= Top {} Job Matches =============\n\n".format(len(matches)))
                
                for i, job in enumerate(matches):
                    f.write(f"Match #{i+1} (Similarity: {job['similarity']:.6f})\n")
                    f.write(f"Title: {job['title']}\n")
                    f.write(f"Location: {job['location']}\n")
                    f.write(f"Source: {job['source']}\n")
                    f.write(f"Skills: {', '.join(job['skills'][:5])}")
                    
                    if len(job['skills']) > 5:
                        f.write(f" and {len(job['skills']) - 5} more")
                    
                    f.write("\n\nDescription Preview:\n")
                    f.write(format_job_description_preview(job['description']))
                    f.write("\n---------------------------------------------\n\n")
        
        print(f"[JobMatcher] Successfully saved {len(matches)} matches to: {output_path}")
    
    except Exception as e:
        print(f"[JobMatcher] Error saving matches to file: {str(e)}")
        raise


def main():
    """
    Main function to find matching jobs for a CV embedding.
    """
    parser = argparse.ArgumentParser(description="Find matching jobs for a CV embedding")
    parser.add_argument("--cv-embedding", type=str, required=True, help="Path to CV embedding JSON file")
    parser.add_argument("--cv-text", type=str, help="Path to CV text file (optional)")
    parser.add_argument("--db-path", type=str, required=True, help="Path to SQLite database with job embeddings")
    parser.add_argument("--output", type=str, required=True, help="Path to save matching jobs file")
    parser.add_argument("--output-format", type=str, default="json", choices=["json", "txt"],
                      help="Output format (json or txt)")
    parser.add_argument("--top-k", type=int, default=5, help="Number of top matches to return")
    parser.add_argument("--min-similarity", type=float, default=0.25, 
                      help="Minimum similarity threshold (0.0 to 1.0)")
    
    args = parser.parse_args()
    
    cv_embedding_path = args.cv_embedding
    cv_text_path = args.cv_text
    db_path = args.db_path
    output_path = args.output
    output_format = args.output_format
    top_k = args.top_k
    min_similarity = args.min_similarity
    
    try:
        print("\n[JobMatcher] Starting job matching process...")
        
        # Load CV embedding
        cv_embedding = load_cv_embedding(cv_embedding_path)
        
        # Load jobs from database
        job_embeddings, job_metadata = load_jobs_from_db(db_path)
        
        # Find matching jobs
        matches = find_matching_jobs(
            cv_embedding, 
            job_embeddings, 
            job_metadata, 
            top_k,
            cv_text_path,
            min_similarity
        )
        
        # Save matches to file
        if matches:
            save_matches_to_file(matches, output_path, output_format)
            
            print("\n[JobMatcher] Job matching process completed successfully.")
            
            # Print top matches
            print("\nTop matching jobs:")
            for i, job in enumerate(matches):
                print(f"{i+1}. {job['title']} - Similarity: {job['similarity']:.4f}")
                print(f"   Location: {job['location']}")
                print(f"   Skills: {', '.join(job['skills'][:5])}" + 
                      (f"... (+{len(job['skills'])-5} more)" if len(job['skills']) > 5 else ""))
                print()
        else:
            print("\n[JobMatcher] No suitable job matches found that meet the minimum similarity threshold.")
            
            # Save empty matches to file
            save_matches_to_file([], output_path, output_format)
        
    except Exception as e:
        print(f"\n[ERROR] {str(e)}")
        exit(1)


if __name__ == "__main__":
    main()