from django.urls import path
from . import views

urlpatterns = [
    path('upload-audio/', views.upload_audio, name='upload_audio'),
    path('broadcast-audio/', views.broadcast_audio, name='broadcast_audio'),
    path('get-conversations/', views.get_conversations, name='get_conversations'),
    path('check-variable/', views.check_variable, name='check_variable'),
]





