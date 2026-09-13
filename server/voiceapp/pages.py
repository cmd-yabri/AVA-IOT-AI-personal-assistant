# voiceapp/pages.py
#
# Browser side of AVA: sign-in / sign-up and a text chat. The ESP32 talks to
# the endpoints in views.py instead and never uses these.

import json

from django.contrib import messages
from django.contrib.auth import authenticate, get_user_model, login
from django.contrib.auth.decorators import login_required
from django.http import JsonResponse
from django.shortcuts import redirect, render
from django.views.decorators.http import require_POST

from .models import Conversation
from . import views


def index(request):
    if request.user.is_authenticated:
        return redirect('chat')
    return render(request, 'voiceapp/index.html')


def signin(request):
    if request.method == 'POST':
        user = authenticate(
            request,
            username=request.POST.get('username', '').strip(),
            password=request.POST.get('password', ''),
        )
        if user is None:
            messages.error(request, 'Invalid username or password.')
            return redirect('signin')
        login(request, user)
        return redirect(request.GET.get('next') or 'chat')
    return render(request, 'voiceapp/signin.html')


def signup(request):
    # One template holds both forms; signin.js shows the right one by path.
    if request.method == 'POST':
        User = get_user_model()
        username = request.POST.get('username', '').strip()
        password = request.POST.get('password', '')
        if not username or not password:
            messages.error(request, 'Username and password are required.')
            return redirect('signup')
        if User.objects.filter(username=username).exists():
            messages.error(request, 'That username is taken.')
            return redirect('signup')
        user = User.objects.create_user(
            username=username,
            email=request.POST.get('email', '').strip(),
            password=password,
        )
        login(request, user)
        return redirect('chat')
    return render(request, 'voiceapp/signin.html')


@login_required
def chat(request):
    history = Conversation.objects.filter(user=request.user)
    return render(request, 'voiceapp/chat.html', {'history': history})


@login_required
@require_POST
def chat_text(request):
    try:
        text = str(json.loads(request.body or b'{}').get('message', '')).strip()
    except (ValueError, AttributeError):
        return JsonResponse({'error': 'Invalid JSON body.'}, status=400)
    if not text:
        return JsonResponse({'error': 'Message is empty.'}, status=400)

    if not views.OPENAI_API_KEY:
        return JsonResponse({'error': 'OPENAI_API_KEY is missing'}, status=500)

    Conversation.objects.create(user=request.user, is_CHATGPT=False, message=text)
    try:
        reply = views.process_with_gpt(text)
    except Exception as e:
        return JsonResponse({'error': f'AI request failed: {e}'}, status=502)
    Conversation.objects.create(user=request.user, is_CHATGPT=True, message=reply)
    return JsonResponse({'reply': reply})


@login_required
@require_POST
def conversations_clear(request):
    Conversation.objects.filter(user=request.user).delete()
    return JsonResponse({'ok': True})
