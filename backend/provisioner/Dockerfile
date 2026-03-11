FROM python:3.11-slim

WORKDIR /app

# Install dependencies
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy application
COPY provisioner.py .

# Run the provisioner
CMD ["python", "-u", "provisioner.py"]
