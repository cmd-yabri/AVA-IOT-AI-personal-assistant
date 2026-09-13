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
from rest_framework.parsers import JSONParser, FormParser, MultiPartParser, FileUploadParser
from rest_framework.renderers import JSONRenderer, BrowsableAPIRenderer

from .models import Conversation
from voiceapp.serializers import ConversationSerializer

from openai import OpenAI

OPENAI_API_KEY = os.getenv("OPENAI_API_KEY")
client = OpenAI(api_key=OPENAI_API_KEY)

MAX_AUDIO_MB = getattr(settings, "MAX_AUDIO_MB", 25)                 # الحد الأقصى لحجم الملف (MB)
CHUNK_SIZE   = getattr(settings, "AUDIO_CHUNK_SIZE", 1024 * 1024)    # حجم التشانك (بايت)
# =========================================

LAST_AUDIO_REL = None 


def process_with_gpt(text: str) -> str:
    """
    يولّد رد نصّي قصير بناء على التفريغ الصوتي.
    """
    resp = client.chat.completions.create(
        model="gpt-4o-mini",
        messages=[{"role": "user", "content": text}],
        max_tokens=60
    )
    return resp.choices[0].message.content


def text_to_speech(text: str) -> str:
    """
    يحوّل النص إلى ملف WAV ويحفظه تحت MEDIA_ROOT/audio/...
    يعيد المسار النسبي داخل media (للتخزين في قاعدة البيانات أو الإرجاع).
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
    يحفظ الـ RAW body (stream) إلى ملف مع حدّ أقصى للحجم.
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


@permission_classes([AllowAny])
@authentication_classes([AllowAny])
@parser_classes([JSONParser, FormParser, MultiPartParser, FileUploadParser])
@renderer_classes([JSONRenderer, BrowsableAPIRenderer])
@api_view(['POST'])
def upload_audio(request):
    """
    يدعم:
      - multipart/form-data (حقل 'audio' أو 'file')
      - RAW audio/wav (عبر FileUploadParser أو wsgi.input)

    السلوك:
      - إذا Accept يحوي audio/wav أو ?format=wav أو X-Return-Audio: 1 → يرجّع WAV مباشرة (مع Content-Length)
      - غير ذلك → JSON فيه transcription و gpt_response
    """
    if not OPENAI_API_KEY:
        return Response({"error": "OPENAI_API_KEY is missing"}, status=status.HTTP_500_INTERNAL_SERVER_ERROR)

    max_bytes = MAX_AUDIO_MB * 1024 * 1024
    temp_dir = os.path.join(settings.MEDIA_ROOT, "temp")
    os.makedirs(temp_dir, exist_ok=True)
    temp_filename = f"temp_{uuid.uuid4()}.wav"
    temp_path = os.path.join(temp_dir, temp_filename)

    audio_file = None
    if hasattr(request, "FILES"):
        audio_file = request.FILES.get('audio') or request.FILES.get('file')

    if audio_file is None and hasattr(request, "data"):
        possible = request.data.get('file') or request.data.get('audio')
        if getattr(possible, 'read', None):
            audio_file = possible

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

        try:
            Conversation.objects.create(
                is_CHATGPT=False,
                message=transcription,
                audio_input=temp_path
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
    يُرجع آخر ملف صوت TTS تم توليده (إذا موجود).
    مفيد للتحميل المنفصل أو الديباغ.
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
    تفقد الجاهزية: True إذا آخر ملف TTS مولّد موجود.
    """
    ready = bool(LAST_AUDIO_REL)
    return Response({"ready": ready}, status=status.HTTP_200_OK)


@api_view(['GET'])
@permission_classes([IsAuthenticated])
def get_conversations(request):
    conversations = Conversation.objects.filter(user=request.user).order_by('-created_at')
    serializer = ConversationSerializer(conversations, many=True)
    return Response(serializer.data, status=status.HTTP_200_OK)
