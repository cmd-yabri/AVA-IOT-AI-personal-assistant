// ========== تبديل النماذج Signin/Signup ==========

// عناصر DOM
const formsContainer = document.getElementById('forms-container'); // عنصر الحاوية للفورمين (signin/signup)
const showSignup = document.getElementById('show-signup');         // رابط → أظهر نموذج التسجيل
const showSignin = document.getElementById('show-signin');         // رابط → أظهر نموذج الدخول

// عند الضغط على رابط التسجيل
if (showSignup) {
  showSignup.addEventListener('click', (e) => {
    // e.preventDefault(); // استعملها إذا بدك التبديل بدون تنقل بين الصفحات
    formsContainer.classList.add('signup-active'); // إضافة كلاس → تفعّل CSS لإظهار signup
  });
}

// عند الضغط على رابط الدخول
if (showSignin) {
  showSignin.addEventListener('click', (e) => {
    // e.preventDefault();
    formsContainer.classList.remove('signup-active'); // إزالة الكلاس → إظهار signin
  });
}

// دالة ذاتية التنفيذ: تفعل signup أو signin تلقائي حسب مسار الرابط
(function autoActivateByPath() {
  const p = (window.location.pathname || '').toLowerCase(); // اجلب مسار الرابط الحالي
  if (p.endsWith('/signup/') || p.includes('/signup')) {    // إذا كان الرابط يشير للتسجيل
    formsContainer.classList.add('signup-active');          // فعّل نموذج signup
  } else {
    formsContainer.classList.remove('signup-active');       // غير ذلك: أبقِ signin
  }
})();


// ========== عرض/إخفاء الباسوورد ==========
document.querySelectorAll('.toggle-password').forEach(btn => {
  // لكل زر تبديل كلمة المرور
  btn.addEventListener('click', () => {
    const targetSel = btn.getAttribute('data-target');  // اجلب الـ selector للحقل المستهدف
    const input = document.querySelector(targetSel);    // حقل كلمة المرور
    if (!input) return;

    const isHidden = input.getAttribute('type') === 'password'; // تحقق إذا الحقل مخفي (password)
    input.setAttribute('type', isHidden ? 'text' : 'password'); // بدّل النوع (text ⇄ password)

    const icon = btn.querySelector('.material-symbols-rounded'); // أيقونة العين
    if (icon) icon.textContent = isHidden ? 'visibility_off' : 'visibility'; // بدّل النص للأيقونة

    // تحديث صفات الوصول (Accessibility)
    btn.setAttribute('aria-label', isHidden ? 'Hide password' : 'Show password');
    btn.setAttribute('aria-pressed', isHidden ? 'true' : 'false');
  });
});
