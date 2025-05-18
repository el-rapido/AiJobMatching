import React, { useEffect, useState } from 'react';

const JobList = () => {
  const [jobs, setJobs] = useState([]);

  useEffect(() => {
    fetch('/jobs.json')
      .then(res => res.json())
      .then(data => setJobs(data))
      .catch(err => console.error('Failed to load jobs:', err));
  }, []);

  return (
    <div className="page" style={{ flexDirection: 'column', alignItems: 'center' }}>
      <h1>Available Jobs</h1>
      {jobs.map((job, index) => (
        <div key={index} className="job-card">
          <h3>{job.title}</h3>
          <p><strong>Description:</strong> {job.description}</p>
          <p><strong>Location:</strong> {job.location}</p>
          <p><strong>Skills:</strong> {job.skills.join(', ')}</p>
          <a href={job.source} target="_blank" rel="noreferrer">
            <button>Apply</button>
          </a>
        </div>
      ))}
    </div>
  );
};

export default JobList;
