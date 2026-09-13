from django.conf import settings
from django.db import models

class Conversation(models.Model):
    # Null for exchanges from the ESP32, which does not sign in.
    user = models.ForeignKey(
        settings.AUTH_USER_MODEL,
        on_delete=models.CASCADE,
        related_name='conversations',
        null=True,
        blank=True,
    )
    session_id = models.CharField(max_length=64, blank=True, default='')
    message = models.TextField()
    audio_input = models.CharField(max_length=255, null=True, blank=True)
    audio_output = models.CharField(max_length=255, null=True, blank=True)
    is_CHATGPT = models.BooleanField(default=False)
    created_at = models.DateTimeField(auto_now_add=True)

    class Meta:
        ordering = ['created_at']

    def __str__(self):
        return self.message[:50]
