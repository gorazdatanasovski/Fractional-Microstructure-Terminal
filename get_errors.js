const puppeteer = require('puppeteer');

(async () => {
    try {
        const browser = await puppeteer.launch();
        const page = await browser.newPage();
        
        page.on('console', msg => {
            if (msg.type() === 'error') {
                console.log('BROWSER ERROR:', msg.text());
            }
        });
        
        page.on('pageerror', err => {
            console.log('PAGE ERROR:', err.toString());
        });
        
        await page.goto('http://localhost:8000', { waitUntil: 'networkidle0' });
        
        // Ensure we scroll to trigger IntersectionObservers
        await page.evaluate(() => {
            window.scrollBy(0, 1000);
        });
        await new Promise(r => setTimeout(r, 1000));
        
        await browser.close();
    } catch (e) {
        console.error("Puppeteer error:", e);
    }
})();
