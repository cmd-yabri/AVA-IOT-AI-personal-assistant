# voiceapp/views.py

import os
import uuid
from django.conf import settings
from django.http import HttpResponse, FileResponse
from rest_framework import status
from rest_framework.response import Response
from rest_framework.decorators import (
    api_view, permission_classes, authentication_classes,
    parser_classes, renderer_classes
)
from rest_framework.permissions import AllowAny, IsAuthenticated
from rest_framework.parsers import JSONParser, FormParser, MultiPartParser
from rest_framework.renderers import JSONRenderer, BrowsableAPIRenderer
from rest_framework.negotiation import DefaultContentNegotiation

from .models import Conversation
from voiceapp.serializers import ConversationSerializer

from openai import OpenAI

OPENAI_API_KEY = os.getenv("OPENAI_API_KEY")
client = OpenAI(api_key=OPENAI_API_KEY) if OPENAI_API_KEY else None

MAX_AUDIO_MB = getattr(settings, "MAX_AUDIO_MB", 25)                 # maximum upload size (MB)
CHUNK_SIZE   = getattr(settings, "AUDIO_CHUNK_SIZE", 1024 * 1024)    # chunk size (bytes)
# =========================================

LAST_AUDIO_REL = None 


def process_with_gpt(text: str) -> str:
    """
    Generate a short text reply to the transcription.
    """
    resp = client.chat.completions.create(
        model="gpt-4o-mini",
        messages=[{"role": "user", "content": text}],
        max_tokens=60
    )
    return resp.choices[0].message.content


def text_to_speech(text: str) -> str:
    """
    Convert text to a WAV file saved under MEDIA_ROOT/audio/...
    Returns the path relative to media (for storing in the database or returning).
    """
    global LAST_AUDIO_REL
    tts = client.audio.speech.create(
        model="tts-1",
        voice="alloy",
        input=text,
        response_format="wav"
    )
    rel = f"audio/response_{uuid.uuid4()}.wav"
    full_path = os.path.join(settings.MEDIA_ROOT, rel)
    os.makedirs(os.path.dirname(full_path), exist_ok=True)
    tts.stream_to_file(full_path)
    LAST_AUDIO_REL = rel
    return rel


def _save_stream_to_file(input_stream, dest_path: str, max_bytes: int):
    """
    Save the raw body (stream) to a file, enforcing a size limit.
    """
    total = 0
    os.makedirs(os.path.dirname(dest_path), exist_ok=True)
    with open(dest_path, "wb") as f:
        while True:
            chunk = input_stream.read(CHUNK_SIZE)
            if not chunk:
                break
            total += len(chunk)
            if total > max_bytes:
                f.close()
                try:
                    os.remove(dest_path)
                except Exception:
                    pass
                return False, total
            f.write(chunk)
    return True, total


class ViewDecidesFormat(DefaultContentNegotiation):
    """
    upload_audio picks WAV or JSON itself (Accept, ?format=wav, X-Return-Audio).
    DRF's default negotiation runs first and answers ?format=wav with 404 and
    Accept: audio/wav with 406, so the view never got the chance.
    """
    def select_renderer(self, request, renderers, format_suffix=None):
        return renderers[0], renderers[0].media_type


def content_negotiation(cls):
    def decorator(func):
        func.content_negotiation_class = cls
        return func
    return decorator


@api_view(['POST'])
@content_negotiation(ViewDecidesFormat)
@permission_classes([AllowAny])
@authentication_classes([])
@parser_classes([JSONParser, FormParser, MultiPartParser])
@renderer_classes([JSONRenderer, BrowsableAPIRenderer])
def upload_audio(request):
    """
    Accepts:
      - multipart/form-data ('audio' or 'file' field)
      - raw audio/wav (read directly from request.stream)

    Behaviour:
      - if Accept contains audio/wav, or ?format=wav, or X-Return-Audio: 1 → return the WAV directly (with Content-Length)
      - otherwise → JSON with transcription and gpt_response
    """
    if not OPENAI_API_KEY:
        return Response({"error": "OPENAI_API_KEY is missing"}, status=status.HTTP_500_INTERNAL_SERVER_ERROR)

    max_bytes = MAX_AUDIO_MB * 1024 * 1024
    temp_dir = os.path.join(settings.MEDIA_ROOT, "temp")
    os.makedirs(temp_dir, exist_ok=True)
    temp_filename = f"temp_{uuid.uuid4()}.wav"
    temp_path = os.path.join(temp_dir, temp_filename)

    audio_file = None
    if (request.content_type or "").startswith("multipart/form-data"):
        audio_file = request.FILES.get('audio') or request.FILES.get('file')
        if audio_file is None:
            return Response({"error": "Send the recording in an 'audio' or 'file' field"},
                            status=status.HTTP_400_BAD_REQUEST)

    if audio_file:
        size_known = getattr(audio_file, 'size', None)
        if size_known is not None and size_known > max_bytes:
            return Response({"error": f"File too large. Max is {MAX_AUDIO_MB}MB"},
                            status=status.HTTP_413_REQUEST_ENTITY_TOO_LARGE)
        written = 0
        with open(temp_path, "wb") as dest:
            if hasattr(audio_file, "chunks"):
                for chunk in audio_file.chunks(CHUNK_SIZE):
                    written += len(chunk)
                    if written > max_bytes:
                        dest.close()
                        try:
                            os.remove(temp_path)
                        except Exception:
                            pass
                        return Response({"error": f"File too large. Max is {MAX_AUDIO_MB}MB"},
                                        status=status.HTTP_413_REQUEST_ENTITY_TOO_LARGE)
                    dest.write(chunk)
            else:
                data = audio_file.read() or b""
                written += len(data)
                if written > max_bytes:
                    return Response({"error": f"File too large. Max is {MAX_AUDIO_MB}MB"},
                                    status=status.HTTP_413_REQUEST_ENTITY_TOO_LARGE)
                dest.write(data)
    else:
        # RAW body
        input_stream = getattr(request, "stream", None) or request.META.get("wsgi.input")
        if input_stream is None:
            return Response({"error": "No audio data found"}, status=status.HTTP_400_BAD_REQUEST)
        ok, _ = _save_stream_to_file(input_stream, temp_path, max_bytes)
        if not ok:
            return Response({"error": f"File too large. Max is {MAX_AUDIO_MB}MB"},
                            status=status.HTTP_413_REQUEST_ENTITY_TOO_LARGE)

    try:
        with open(temp_path, "rb") as audio:
            transcription = client.audio.transcriptions.create(
                file=audio,
                model="whisper-1",
                response_format="text"
            )

        audio_input_rel = f"audio/input_{uuid.uuid4()}.wav"
        audio_input_abs = os.path.join(settings.MEDIA_ROOT, audio_input_rel)
        os.makedirs(os.path.dirname(audio_input_abs), exist_ok=True)
        os.replace(temp_path, audio_input_abs)

        try:
            Conversation.objects.create(
                is_CHATGPT=False,
                message=transcription,
                audio_input=audio_input_rel
            )
        except Exception:
            pass

        gpt_response = process_with_gpt(transcription)
        audio_response_rel = text_to_speech(gpt_response)
        audio_abs = os.path.join(settings.MEDIA_ROOT, audio_response_rel)

        try:
            Conversation.objects.create(
                is_CHATGPT=True,
                message=gpt_response,
                audio_output=audio_response_rel
            )
        except Exception:
            pass

        accept = (request.META.get("HTTP_ACCEPT", "") or "").lower()
        return_wav = (
            "audio/wav" in accept or
            "audio/*" in accept or
            request.GET.get("format") == "wav" or
            request.META.get("HTTP_X_RETURN_AUDIO") == "1"
        )

        if return_wav:
            try:
                with open(audio_abs, "rb") as f:
                    data = f.read()
                resp = HttpResponse(data, content_type="audio/wav")
                resp["Content-Disposition"] = 'inline; filename="response.wav"'
                resp["Content-Length"] = str(len(data))
                resp["X-Transcription"] = str(transcription)[:200].replace("\n", " ")
                resp["X-GPT-Text"] = gpt_response[:200].replace("\n", " ")
                resp["Cache-Control"] = "no-store"
                return resp
            except Exception as e:
                return Response({"error": f"Audio open failed: {e}"},
                                status=status.HTTP_500_INTERNAL_SERVER_ERROR)

        return Response(
            {"transcription": transcription, "gpt_response": gpt_response},
            status=status.HTTP_200_OK
        )

    except Exception as e:
        return Response({"error": str(e)}, status=status.HTTP_500_INTERNAL_SERVER_ERROR)

    finally:
        try:
            if os.path.exists(temp_path):
                os.remove(temp_path)
        except Exception:
            pass


@api_view(['GET'])
def broadcast_audio(request):
    """
    Return the most recently generated TTS audio file (if any).
    Useful for a separate download or for debugging.
    """
    global LAST_AUDIO_REL
    if not LAST_AUDIO_REL:
        return Response({"error": "No audio file available"}, status=status.HTTP_404_NOT_FOUND)

    audio_abs = os.path.join(settings.MEDIA_ROOT, LAST_AUDIO_REL)
    if not os.path.exists(audio_abs):
        return Response({"error": "Audio file not found"}, status=status.HTTP_404_NOT_FOUND)

    try:
        f = open(audio_abs, "rb")
        resp = FileResponse(f, content_type="audio/wav")
        resp["Content-Disposition"] = 'attachment; filename="response.wav"'
        resp["Content-Length"] = str(os.path.getsize(audio_abs))
        return resp
    except Exception as e:
        return Response({"error": str(e)}, status=status.HTTP_500_INTERNAL_SERVER_ERROR)


@api_view(['GET'])
def check_variable(request):
    """
    Readiness check: True if a generated TTS file exists.
    """
    ready = bool(LAST_AUDIO_REL)
    return Response({"ready": ready}, status=status.HTTP_200_OK)


@api_view(['GET'])
@permission_classes([IsAuthenticated])
def get_conversations(request):
    conversations = Conversation.objects.filter(user=request.user).order_by('-created_at')
    serializer = ConversationSerializer(conversations, many=True)
    return Response(serializer.data, status=status.HTTP_200_OK)
