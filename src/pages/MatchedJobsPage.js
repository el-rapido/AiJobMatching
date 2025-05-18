import React, { useEffect, useState } from 'react';
import { getMatchedJobs } from '../services/api';
import JobCard from '../components/JobCard';

const MatchedJobsPage = () => {
  const [matches, setMatches] = useState([]);

  useEffect(() => {
    const fetchMatches = async () => {
      const data = await getMatchedJobs();
      setMatches(data.jobs);
    };
    fetchMatches();
  }, []);

  return (
    <div className="page">
      <h2>Matched Jobs</h2>
      {matches.length > 0 ? (
        matches.map((job) => <JobCard key={job.id} job={job} />)
      ) : (
        <p>No matched jobs yet.</p>
      )}
    </div>
  );
};

export default MatchedJobsPage;
