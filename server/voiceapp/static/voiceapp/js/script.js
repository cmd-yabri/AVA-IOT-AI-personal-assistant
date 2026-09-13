// static/voiceapp/js/script.js

document.addEventListener("DOMContentLoaded", () => {
  // wait for the DOM, then set up references and event handlers

  const chatsContainer = document.querySelector(".chats-container"); // chat messages container
  const promptForm = document.querySelector(".prompt-form");         // bottom input form
  const promptInput = promptForm ? promptForm.querySelector(".prompt-input") : null; // text input, if present
  const themeToggle = document.querySelector("#theme-toggle-btn");   // theme toggle button
  const deleteBtn = document.querySelector("#delete-chat-btn");      // clear chat button
  const suggestions = document.querySelector(".suggestions");        // initial suggestions list

  // === CSRF ===
  function getCSRFCookie(name = "csrftoken") {
    // read the CSRF cookie with the given name
    const v = document.cookie.split(";").map(c => c.trim()); // split cookies into key/value pairs
    for (const c of v) {
      if (c.startsWith(name + "=")) return decodeURIComponent(c.slice(name.length + 1)); // return the value if found
    }
    return null; // no cookie with that name
  }

  // === Theme (restore + toggle)
  try {
    const saved = localStorage.getItem("ava_theme"); // restore the saved theme (light|dark)
    if (saved === "light") {
      document.body.classList.add("light-theme"); // enable light mode
      if (themeToggle) themeToggle.textContent = "dark_mode"; // matching icon
    }
  } catch (_) {} // ignore localStorage access errors

  if (themeToggle) {
    themeToggle.addEventListener("click", () => {
      // toggle the theme on click
      const isLight = document.body.classList.toggle("light-theme"); // add/remove the theme class
      themeToggle.textContent = isLight ? "dark_mode" : "light_mode"; // swap the icon
      try { localStorage.setItem("ava_theme", isLight ? "light" : "dark"); } catch (_) {} // save the choice
    });
  }

  // === Helpers ===
  const createMsgElement = (content, ...classes) => {
    // create a message element with HTML content and extra classes
    const div = document.createElement("div");
    div.classList.add("message", ...classes);
    div.innerHTML = content;
    return div;
  };

  const typeText = (el, text, delay = 18) => {
    // typewriter effect for text inside an element
    if (!el) return;
    let i = 0;
    const it = setInterval(() => {
      el.textContent += text.charAt(i); // add one character at a time
      if (++i >= text.length) clearInterval(it); // stop when the text is done
    }, delay);
  };

  // hide/show suggestions + mark that a chat exists
  function updateSuggestionsVisibility() {
    // decide whether suggestions show, based on messages in the container
    if (!suggestions || !chatsContainer) return;
    const hasMessages = !!chatsContainer.querySelector(".message");
    suggestions.classList.toggle("is-hidden", hasMessages); // hide suggestions when there are messages
    document.body.classList.toggle("chats-active", hasMessages); // add the state class
  }
  // initial call (covers saved history)
  updateSuggestionsVisibility();

  function ensureChatsActive() {
    // force the "chat exists" state in some browsers to avoid odd behaviour
    document.body.classList.add("chats-active");
    suggestions?.classList.add("is-hidden");
  }

  const botBubble = (text = "just a sec...") => {
    // create a placeholder (loading) bot bubble and return its element
    const avatarSrc = window.STATIC_AVATAR || "/static/voiceapp/images/ava.png"; // image path
    const html = `
      <img src="${avatarSrc}" class="avatar" alt="AVA">
      <p class="message-text">${text}</p>
    `;
    const div = createMsgElement(html, "bot-message"); // create a bot message
    chatsContainer.appendChild(div);                   // append it to the chat
    ensureChatsActive();
    // use rAF, then check (handles update races on Edge)
    requestAnimationFrame(updateSuggestionsVisibility);
    return div; // return the message element so it can be updated later
  };

  const showError = (msg) => {
    // show an error message as text in a bot bubble
    const div = createMsgElement(`<p class="message-text">⚠️ ${msg}</p>`, "bot-message");
    chatsContainer.appendChild(div);
    ensureChatsActive();
    requestAnimationFrame(updateSuggestionsVisibility);
  };

  // === Send text to server and show reply
  const sendTextToServer = async (userText) => {
    // send the user's text to the API and render the reply with a typing effect
    const loadingDiv = botBubble("just a sec..."); // waiting bubble
    try {
      const url = window.CHAT_TEXT_URL || "/chat-text/"; // default API path if the template did not provide one
      const res = await fetch(url, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          "X-Requested-With": "XMLHttpRequest",
          "X-CSRFToken": getCSRFCookie(), // CSRF protection
        },
        body: JSON.stringify({ message: userText }), // payload: the user's message
        credentials: "same-origin", // send cookies for same-origin requests
      });

      if (!res.ok) {
        // try to read the error message from JSON and throw it
        const err = await res.json().catch(() => ({}));
        throw new Error(err.error || `HTTP ${res.status}`);
      }

      const data = await res.json(); // parse the response
      const reply = (data && data.reply) ? String(data.reply) : "(no reply)"; // backend reply text
      const p = loadingDiv.querySelector(".message-text"); // text element in the waiting bubble
      if (p) { p.textContent = ""; typeText(p, reply, 18); } // type out the reply
      ensureChatsActive();
      requestAnimationFrame(updateSuggestionsVisibility);
    } catch (e) {
      loadingDiv.remove(); // remove the waiting bubble on error
      showError(e.message || "Server error"); // show the error
    }
  };

  // === Handle form submit
  const handleFormSubmit = (e) => {
    // handle form submit: add the user bubble and send the request to the server
    e.preventDefault();
    if (!promptInput) return;
    const text = promptInput.value.trim();
    if (!text) return;

    promptInput.value = ""; // clear the input after sending

    // user bubble
    const userDiv = createMsgElement('<p class="message-text"></p>', "user-message"); // user bubble
    userDiv.querySelector(".message-text").textContent = text; // set the text
    chatsContainer.appendChild(userDiv); // append it to the chat
    ensureChatsActive();
    requestAnimationFrame(updateSuggestionsVisibility);

    // send to server
    sendTextToServer(text); // call the API
  };
  if (promptForm) promptForm.addEventListener("submit", handleFormSubmit); // bind the handler to the form

  // === Suggestions click (fill in and send)
  document.querySelectorAll(".suggestions-item").forEach((item) => {
    // for each suggestion: fill the input with its text and submit the form
    item.addEventListener("click", () => {
      const t = item.querySelector(".text");
      if (!t || !promptInput || !promptForm) return;
      promptInput.value = t.textContent || "";
      promptForm.dispatchEvent(new Event("submit")); // simulate a form submit
    });
  });

  // === Clear chat (DB + UI)
  if (deleteBtn) {
    deleteBtn.addEventListener("click", async () => {
      // delete conversations on the server and clean up the UI
      try {
        const sure = confirm("Delete the whole conversation?"); // ask the user to confirm
        if (!sure) return;

        const clearUrl = window.CLEAR_URL || "/api/conversations/clear/"; // API path for clearing
        const res = await fetch(clearUrl, {
          method: "POST",
          headers: {
            "X-CSRFToken": getCSRFCookie(),
            "X-Requested-With": "XMLHttpRequest"
          },
          credentials: "same-origin"
        });

        const data = await res.json().catch(() => ({})); // server response
        if (!res.ok || !data.ok) {
          throw new Error(data.error || `HTTP ${res.status}`); // throw the error, if any
        }

        // clean up the UI + show suggestions
        if (chatsContainer) chatsContainer.innerHTML = ""; // remove the messages
        document.body.classList.remove("chats-active");    // remove the chat state
        suggestions?.classList.remove("is-hidden");        // show suggestions
        console.debug(`[AVA] deleted ${data.deleted} messages`); // diagnostic log
      } catch (e) {
        alert("Could not clear the chat: " + (e.message || "Server error")); // alert the error
      }
    });
  }

  console.debug("[AVA] chat script loaded."); // log once the script has loaded
});
