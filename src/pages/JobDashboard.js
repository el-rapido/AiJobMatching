import React, { useEffect, useState } from 'react';
import { getJobs } from '../services/api';
import JobCard from '../components/JobCard';

const JobDashboard = () => {
  const [jobs, setJobs] = useState([]);

  useEffect(() => {
    const fetchJobs = async () => {
      const data = await getJobs();
      setJobs(data.jobs);
    };
    fetchJobs();
  }, []);

  return (
    <div className="page">
      <h2>Job Listings</h2>
      {jobs.length > 0 ? (
        jobs.map((job) => <JobCard key={job.id} job={job} />)
      ) : (
        <p>Loading jobs...</p>
      )}
    </div>
  );
};

export default JobDashboard;
