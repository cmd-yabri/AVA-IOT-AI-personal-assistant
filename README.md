# AVA IOT AI Personal Assistant

AVA is a voice-enabled personal assistant that connects an ESP32 device to a Django backend for speech-to-text, GPT responses, and text-to-speech audio playback.

**Project Structure**
- `frontend&backend/` Django backend and web UI
- `electronics code/` ESP32 firmware (audio capture, upload, and playback)

**Requirements**
- Python 3.x
- Python packages: `django`, `djangorestframework`, `openai`
- An OpenAI API key in `OPENAI_API_KEY`

**Backend Setup**
1. Open a terminal in `frontend&backend/`
2. Install dependencies:
   ```bash
   pip install django djangorestframework openai
   ```
3. Set your API key:
   ```bash
   # PowerShell
   $env:OPENAI_API_KEY="your_key_here"
   ```
4. Apply migrations and run:
   ```bash
   python manage.py migrate
   python manage.py runserver 0.0.0.0:8000
   ```

**API Endpoints**
- `POST /upload-audio/`
  - Accepts `multipart/form-data` (`audio` or `file`) or raw `audio/wav`.
  - Returns JSON by default.
  - To get WAV back, send `Accept: audio/wav` or add `?format=wav` or `X-Return-Audio: 1`.
- `GET /broadcast-audio/` returns the last TTS audio file (if available).
- `GET /check-variable/` readiness check.
- `GET /get-conversations/` authenticated conversation history.

**Quick Test (raw WAV)**
```bash
curl -X POST -H "Content-Type: audio/wav" --data-binary @path/to/file.wav http://localhost:8000/upload-audio/
```

**Notes**
- Do not commit API keys. Use environment variables.
- The ESP32 firmware entrypoint is `electronics code/hhh.ino`.
