const API_BASE = 'http://localhost:8000'; // Update this if backend URL changes

// Upload CV and return matched jobs in response
export const uploadCVAndGetMatches = async (file) => {
  const formData = new FormData();
  formData.append('cv', file);

  const response = await fetch(`${API_BASE}/upload-cv`, {
    method: 'POST',
    body: formData,
  });

  if (!response.ok) {
    throw new Error('Failed to upload CV and fetch matched jobs');
  }

  return response.json(); // Expecting: { message, jobs: [...] }
};

// Fetch random/all jobs (for browsing before uploading CV)
export const getJobs = async () => {
  const response = await fetch(`${API_BASE}/jobs`);

  if (!response.ok) {
    throw new Error('Failed to fetch job listings');
  }

  return response.json(); // Expecting: { jobs: [...] }
};
