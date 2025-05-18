import React, { useState } from 'react';
import { uploadCV } from '../services/api';

const UploadCVPage = () => {
  const [file, setFile] = useState(null);
  const [message, setMessage] = useState('');

  const handleFileChange = (e) => {
    setFile(e.target.files[0]);
  };

  const handleUpload = async () => {
    if (!file) {
      setMessage('Please select a file first.');
      return;
    }
    const result = await uploadCV(file);
    setMessage(result.message);
  };

  return (
    <div className="page">
      <h2>Upload Your CV</h2>
      <input type="file" onChange={handleFileChange} />
      <button onClick={handleUpload}>Upload</button>
      {message && <p>{message}</p>}
    </div>
  );
};

export default UploadCVPage;
