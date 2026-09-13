from django.contrib.auth.views import LogoutView
from django.urls import path
from . import pages, views

urlpatterns = [
    # ESP32 device API
    path('upload-audio/', views.upload_audio, name='upload_audio'),
    path('broadcast-audio/', views.broadcast_audio, name='broadcast_audio'),
    path('get-conversations/', views.get_conversations, name='get_conversations'),
    path('check-variable/', views.check_variable, name='check_variable'),

    # Browser
    path('', pages.index, name='index'),
    path('signin/', pages.signin, name='signin'),
    path('signup/', pages.signup, name='signup'),
    path('signout/', LogoutView.as_view(next_page='signin'), name='signout'),
    path('chat/', pages.chat, name='chat'),
    path('chat-text/', pages.chat_text, name='chat_text'),
    path('api/conversations/clear/', pages.conversations_clear, name='api_conversations_clear'),
]
