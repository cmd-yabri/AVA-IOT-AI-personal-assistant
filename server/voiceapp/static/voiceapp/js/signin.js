// ========== Sign-in / sign-up form switching ==========

// DOM elements
const formsContainer = document.getElementById('forms-container'); // container for both forms (signin/signup)
const showSignup = document.getElementById('show-signup');         // link → show the sign-up form
const showSignin = document.getElementById('show-signin');         // link → show the sign-in form

// on clicking the sign-up link
if (showSignup) {
  showSignup.addEventListener('click', (e) => {
    // e.preventDefault(); // use this to switch forms without navigating between pages
    formsContainer.classList.add('signup-active'); // add the class → CSS shows sign-up
  });
}

// on clicking the sign-in link
if (showSignin) {
  showSignin.addEventListener('click', (e) => {
    // e.preventDefault();
    formsContainer.classList.remove('signup-active'); // remove the class → show sign-in
  });
}

// self-invoking function: pick signup or signin automatically from the URL path
(function autoActivateByPath() {
  const p = (window.location.pathname || '').toLowerCase(); // get the current URL path
  if (p.endsWith('/signup/') || p.includes('/signup')) {    // if the URL points to sign-up
    formsContainer.classList.add('signup-active');          // activate the sign-up form
  } else {
    formsContainer.classList.remove('signup-active');       // otherwise: keep sign-in
  }
})();


// ========== Show/hide password ==========
document.querySelectorAll('.toggle-password').forEach(btn => {
  // for each password toggle button
  btn.addEventListener('click', () => {
    const targetSel = btn.getAttribute('data-target');  // get the selector of the target field
    const input = document.querySelector(targetSel);    // password field
    if (!input) return;

    const isHidden = input.getAttribute('type') === 'password'; // check whether the field is hidden (password)
    input.setAttribute('type', isHidden ? 'text' : 'password'); // swap the type (text ⇄ password)

    const icon = btn.querySelector('.material-symbols-rounded'); // eye icon
    if (icon) icon.textContent = isHidden ? 'visibility_off' : 'visibility'; // swap the icon text

    // update accessibility attributes
    btn.setAttribute('aria-label', isHidden ? 'Hide password' : 'Show password');
    btn.setAttribute('aria-pressed', isHidden ? 'true' : 'false');
  });
});
