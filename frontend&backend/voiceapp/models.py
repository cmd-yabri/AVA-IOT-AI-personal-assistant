from django.db import models

class Conversation(models.Model):
    message = models.TextField()
    audio_input = models.CharField(max_length=255, null=True, blank=True)
    audio_output = models.CharField(max_length=255, null=True, blank=True)
    is_CHATGPT = models.BooleanField(default=False)
    created_at = models.DateTimeField(auto_now_add=True)

    def __str__(self):
        return self.message[:50]
