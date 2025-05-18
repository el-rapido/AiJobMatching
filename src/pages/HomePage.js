import React, { useState } from 'react';
import { uploadCVAndGetMatches } from '../services/api';
import JobCard from '../components/JobCard';

const HomePage = () => {
  const [file, setFile] = useState(null);
  const [message, setMessage] = useState('');
  const [matches, setMatches] = useState([]);

  const handleFileChange = (e) => {
    setFile(e.target.files[0]);
  };

  const handleUpload = async () => {
    if (!file) {
      setMessage('Please select a file first.');
      return;
    }

    try {
      const result = await uploadCVAndGetMatches(file);
      setMessage(result.message || 'Upload successful!');
      setMatches(result.jobs || []);
    } catch (error) {
      console.error('Error uploading CV:', error);
      setMessage('Failed to upload CV. Please try again.');
    }
  };

  return (
    <div className="page">
      {/* Left Column: Text Content */}
      <div className="left-content">
        <h1>Welcome to AI Job Matcher</h1>
        <p>Upload your CV to find matching jobs instantly!</p>
        {matches.length > 0 && (
          <>
            <h2>Matched Jobs</h2>
            {matches.map((job) => (
              <JobCard key={job.id} job={job} showApply={true} />
            ))}
          </>
        )}
        {matches.length === 0 && message && (
          <p>No matches found. Try uploading a different CV.</p>
        )}
      </div>

      {/* Right Column: Upload Box */}
      <div className="right-content">
        <input type="file" onChange={handleFileChange} />
        <button onClick={handleUpload}>Upload CV</button>
        {message && <p>{message}</p>}
      </div>
    </div>
  );
};

export default HomePage;
