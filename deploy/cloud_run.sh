#!/usr/bin/env bash
# Deploy the image to Google Cloud Run. The image is copied from the GitLab registry to
# Artifact Registry, because Cloud Run pulls from Google registries.
set -euo pipefail
REGION="${GCP_REGION:-europe-west1}"
REPO="${GCP_REGION:-europe-west1}-docker.pkg.dev/$GCP_PROJECT_ID/engine/matching-engine"
gcloud auth configure-docker "${REGION}-docker.pkg.dev" --quiet
gcloud artifacts repositories describe engine --location "$REGION" >/dev/null 2>&1 || \
  gcloud artifacts repositories create engine --repository-format docker --location "$REGION"
gcloud builds submit --tag "$REPO:$CI_COMMIT_SHORT_SHA" . --quiet
gcloud run deploy matching-engine --image "$REPO:$CI_COMMIT_SHORT_SHA" --region "$REGION" \
  --allow-unauthenticated --memory 512Mi --cpu 1 --max-instances 2 \
  --set-env-vars "APP_VERSION=$CI_COMMIT_SHORT_SHA,VERIFY_ORDERS=50000" --quiet
gcloud run services describe matching-engine --region "$REGION" --format 'value(status.url)'
