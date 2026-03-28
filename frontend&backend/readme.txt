curl -X POST -H "Content-Type: audio/wav" --data-binary @test2.wav http://localhost:8000/upload-audio/
python manage.py runserver 0.0.0.0:8000
http://localhost:8000/broadhttp://localhost:8000/upload-audio/
cast-audio/?format=api