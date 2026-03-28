// static/voiceapp/js/script.js

document.addEventListener("DOMContentLoaded", () => {
  // ينتظر تحميل الـ DOM ثم يهيّئ المراجع والأحداث

  const chatsContainer = document.querySelector(".chats-container"); // حاوية رسائل الدردشة
  const promptForm = document.querySelector(".prompt-form");         // نموذج الإدخال السفلي
  const promptInput = promptForm ? promptForm.querySelector(".prompt-input") : null; // حقل النص إن وُجد
  const themeToggle = document.querySelector("#theme-toggle-btn");   // زر تبديل الثيم
  const deleteBtn = document.querySelector("#delete-chat-btn");      // زر حذف المحادثة
  const suggestions = document.querySelector(".suggestions");        // قائمة الاقتراحات الأولية

  // === CSRF ===
  function getCSRFCookie(name = "csrftoken") {
    // يجلب قيمة كوكي CSRF بالاسم المعطى
    const v = document.cookie.split(";").map(c => c.trim()); // يقسم الكوكيز إلى مفاتيح/قيم
    for (const c of v) {
      if (c.startsWith(name + "=")) return decodeURIComponent(c.slice(name.length + 1)); // يعيد القيمة إن وُجدت
    }
    return null; // لا يوجد كوكي بهذا الاسم
  }

  // === Theme (restore + toggle)
  try {
    const saved = localStorage.getItem("ava_theme"); // استرجاع الثيم المحفوظ (light|dark)
    if (saved === "light") {
      document.body.classList.add("light-theme"); // تفعيل نمط الفاتح
      if (themeToggle) themeToggle.textContent = "dark_mode"; // أيقونة مناسبة
    }
  } catch (_) {} // تجاهل أخطاء الوصول إلى التخزين المحلي

  if (themeToggle) {
    themeToggle.addEventListener("click", () => {
      // تبديل الثيم عند النقر
      const isLight = document.body.classList.toggle("light-theme"); // يضيف/يزيل كلاس الثيم
      themeToggle.textContent = isLight ? "dark_mode" : "light_mode"; // يبدّل الأيقونة
      try { localStorage.setItem("ava_theme", isLight ? "light" : "dark"); } catch (_) {} // يحفظ الاختيار
    });
  }

  // === Helpers ===
  const createMsgElement = (content, ...classes) => {
    // ينشئ عنصر رسالة عامة مع محتوى HTML وكلاسات إضافية
    const div = document.createElement("div");
    div.classList.add("message", ...classes);
    div.innerHTML = content;
    return div;
  };

  const typeText = (el, text, delay = 18) => {
    // تأثير كتابة تدريجية للنص داخل عنصر
    if (!el) return;
    let i = 0;
    const it = setInterval(() => {
      el.textContent += text.charAt(i); // يضيف حرفًا حرفًا
      if (++i >= text.length) clearInterval(it); // يوقف عند انتهاء النص
    }, delay);
  };

  // إخفاء/إظهار الاقتراحات + علامة وجود محادثة
  function updateSuggestionsVisibility() {
    // يقرر إن كانت الاقتراحات تظهر بناء على وجود رسائل في الحاوية
    if (!suggestions || !chatsContainer) return;
    const hasMessages = !!chatsContainer.querySelector(".message");
    suggestions.classList.toggle("is-hidden", hasMessages); // إخفاء الاقتراحات عند وجود رسائل
    document.body.classList.toggle("chats-active", hasMessages); // إضافة كلاس حالة
  }
  // استدعاء أولي (يغطي history)
  updateSuggestionsVisibility();

  function ensureChatsActive() {
    // يفرض حالة "هناك محادثة" لبعض المتصفحات لتجنّب سلوك غريب
    document.body.classList.add("chats-active");
    suggestions?.classList.add("is-hidden");
  }

  const botBubble = (text = "just a sec...") => {
    // ينشئ فقاعة رسالة بوت مبدئية (تحميل) ويعيد عنصرها
    const avatarSrc = window.STATIC_AVATAR || "/static/voiceapp/images/ava.png"; // مسار الصورة
    const html = `
      <img src="${avatarSrc}" class="avatar" alt="AVA">
      <p class="message-text">${text}</p>
    `;
    const div = createMsgElement(html, "bot-message"); // إنشاء رسالة بوت
    chatsContainer.appendChild(div);                   // إضافتها للمحادثة
    ensureChatsActive();
    // نستخدم rAF ثم التحقق (يعالج سباقات التحديث على Edge)
    requestAnimationFrame(updateSuggestionsVisibility);
    return div; // إعادة عنصر الرسالة لتحديثه لاحقًا
  };

  const showError = (msg) => {
    // يعرض رسالة خطأ كنص في فقاعة بوت
    const div = createMsgElement(`<p class="message-text">⚠️ ${msg}</p>`, "bot-message");
    chatsContainer.appendChild(div);
    ensureChatsActive();
    requestAnimationFrame(updateSuggestionsVisibility);
  };

  // === Send text to server and show reply
  const sendTextToServer = async (userText) => {
    // يرسل نص المستخدم إلى API ويرسم رد البوت مع تأثير الكتابة
    const loadingDiv = botBubble("just a sec..."); // فقاعة انتظار
    try {
      const url = window.CHAT_TEXT_URL || "/chat-text/"; // مسار API الافتراضي إن لم يُمرَّر من القالب
      const res = await fetch(url, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          "X-Requested-With": "XMLHttpRequest",
          "X-CSRFToken": getCSRFCookie(), // حماية CSRF
        },
        body: JSON.stringify({ message: userText }), // الحمولة: رسالة المستخدم
        credentials: "same-origin", // تضمين الكوكيز مع نفس الأصل
      });

      if (!res.ok) {
        // يحاول قراءة رسالة الخطأ من JSON وإلقائها
        const err = await res.json().catch(() => ({}));
        throw new Error(err.error || `HTTP ${res.status}`);
      }

      const data = await res.json(); // تفريغ الردّ
      const reply = (data && data.reply) ? String(data.reply) : "(no reply)"; // نص ردّ الباك-إند
      const p = loadingDiv.querySelector(".message-text"); // عنصر النص في فقاعة الانتظار
      if (p) { p.textContent = ""; typeText(p, reply, 18); } // تأثير كتابة الرد
      ensureChatsActive();
      requestAnimationFrame(updateSuggestionsVisibility);
    } catch (e) {
      loadingDiv.remove(); // إزالة فقاعة الانتظار عند الخطأ
      showError(e.message || "Server error"); // عرض الخطأ
    }
  };

  // === Handle form submit
  const handleFormSubmit = (e) => {
    // يعالج إرسال النموذج: يضيف فقاعة المستخدم ويرسل الطلب للسيرفر
    e.preventDefault();
    if (!promptInput) return;
    const text = promptInput.value.trim();
    if (!text) return;

    promptInput.value = ""; // إفراغ الحقل بعد الإرسال

    // user bubble
    const userDiv = createMsgElement('<p class="message-text"></p>', "user-message"); // فقاعة المستخدم
    userDiv.querySelector(".message-text").textContent = text; // وضع النص
    chatsContainer.appendChild(userDiv); // إضافتها للمحادثة
    ensureChatsActive();
    requestAnimationFrame(updateSuggestionsVisibility);

    // send to server
    sendTextToServer(text); // استدعاء API
  };
  if (promptForm) promptForm.addEventListener("submit", handleFormSubmit); // ربط الحدث بالنموذج

  // === Suggestions click (املأ وابعث)
  document.querySelectorAll(".suggestions-item").forEach((item) => {
    // لكل عنصر اقتراح: املأ الحقل بنفس النص وأرسل النموذج
    item.addEventListener("click", () => {
      const t = item.querySelector(".text");
      if (!t || !promptInput || !promptForm) return;
      promptInput.value = t.textContent || "";
      promptForm.dispatchEvent(new Event("submit")); // محاكاة إرسال النموذج
    });
  });

  // === Clear chat (DB + UI)
  if (deleteBtn) {
    deleteBtn.addEventListener("click", async () => {
      // يحذف المحادثات من الخادوم وينظّف الواجهة
      try {
        const sure = confirm("هل تريد حذف كل المحادثة؟"); // تأكيد المستخدم
        if (!sure) return;

        const clearUrl = window.CLEAR_URL || "/api/conversations/clear/"; // مسار API للحذف
        const res = await fetch(clearUrl, {
          method: "POST",
          headers: {
            "X-CSRFToken": getCSRFCookie(),
            "X-Requested-With": "XMLHttpRequest"
          },
          credentials: "same-origin"
        });

        const data = await res.json().catch(() => ({})); // ردّ السيرفر
        if (!res.ok || !data.ok) {
          throw new Error(data.error || `HTTP ${res.status}`); // رفع الخطأ إن وجد
        }

        // تنظيف الواجهة + إظهار الاقتراحات
        if (chatsContainer) chatsContainer.innerHTML = ""; // إزالة الرسائل
        document.body.classList.remove("chats-active");    // إزالة حالة المحادثة
        suggestions?.classList.remove("is-hidden");        // إظهار الاقتراحات
        console.debug(`[AVA] deleted ${data.deleted} messages`); // لوج تشخيصي
      } catch (e) {
        alert("تعذّر الحذف: " + (e.message || "Server error")); // تنبيه بالخطأ
      }
    });
  }

  console.debug("[AVA] chat script loaded."); // رسالة لوج عند اكتمال تحميل السكربت
});
