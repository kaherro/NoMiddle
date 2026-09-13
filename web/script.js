(() => {
    const baseUrl = window.location.origin;
    const api = {
        register: `${baseUrl}/api/auth/register`,
        publicKey: `${baseUrl}/api/public_key`,
        contacts: `${baseUrl}/api/contacts`,
        messages: `${baseUrl}/api/messages`,
        sendMessage: `${baseUrl}/api/send_message`,
        upsertContact: `${baseUrl}/api/upsert_contact`,
        fetchRemotePublicKey: (addr) => {
            let url = addr;
            if (url.startsWith('http://')) {
                url = url.substring(7);
            }
            else if (url.startsWith('https://')) {
                url = url.substring(8);
            }
            if (url.endsWith('/')) {
                url = url.slice(0, -1);
            }
            return `https://${url}/api/public_key`;
        }
    };

    const AUTH_ID_KEY = 'nmd_client_id';
    const AUTH_SECRET_KEY = 'nmd_client_secret';

    let authId = localStorage.getItem(AUTH_ID_KEY);
    let authSecret = localStorage.getItem(AUTH_SECRET_KEY);

    const lockScreenEl = document.getElementById('lock-screen');
    const lockMessageEl = document.getElementById('lock-message');
    const lockRefreshBtn = document.getElementById('lock-refresh');

    function showLockScreen(message) {
        lockMessageEl.textContent = message;
        lockScreenEl.style.display = 'flex';
    }

    lockRefreshBtn.addEventListener('click', () => { location.reload(); });

    function apiFetch(url, options) {
        const opts = options || {};
        opts.headers = Object.assign({}, opts.headers);
        if (authId && authSecret) {
            opts.headers['X-Client-Id'] = authId;
            opts.headers['X-Client-Secret'] = authSecret;
        }
        return fetch(url, opts).then(resp => {
            if (resp.status === 401) {
                localStorage.removeItem(AUTH_ID_KEY);
                localStorage.removeItem(AUTH_SECRET_KEY);
                authId = null;
                authSecret = null;
                showLockScreen('Your session was revoked. This device can no longer access the node.');
            }
            return resp;
        });
    }

    async function registerClient() {
        try {
            const resp = await fetch(api.register, { method: 'POST' });
            if (resp.status === 403) {
                showLockScreen('This node is locked. Ask the owner to register your device.');
                return false;
            }
            if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
            const data = await resp.json();
            authId = data.client_id;
            authSecret = data.secret;
            localStorage.setItem(AUTH_ID_KEY, authId);
            localStorage.setItem(AUTH_SECRET_KEY, authSecret);
            return true;
        } 
        catch (err) {
            console.error('Failed to register client:', err);
            showLockScreen('Failed to connect to the server.');
            return false;
        }
    }

    let selfPublicKey = null;
    let selectedContactId = null;
    let lastContacts = [];
    let pendingSelfMessageId = null;

    const messengerWindowEl = document.getElementById('messenger-window');

    const myDomainValueEl = document.getElementById('my-domain-value');
    const myDomainCopyBtn = document.getElementById('my-domain-copy');
    const contactsListEl = document.getElementById('contacts-list');
    const contactNameTextEl = document.getElementById('contact-name-text');
    const chatHeaderEl = document.getElementById('contact-name-right');
    const chatSettingsBtn = document.getElementById('chat-settings-button');
    const messageFieldEl = document.getElementById('message-field');
    const messagesListEl = document.getElementById('messages-list');
    const messageInputEl = document.getElementById('message-text');
    const sendButtonEl = document.getElementById('send-button');
    const addContactBtnEl = document.getElementById('add-contact-button');
    const addContactOverlayEl = document.querySelector('.modal-overlay');
    const addContactWindowEl = document.getElementById('add-contact-window');
    const addContactNameInput = document.getElementById('add-contact-input-name');
    const addContactIpInput = document.getElementById('add-contact-input-ip');
    const addContactConfirmBtn = document.getElementById('add-contact-confirm');
    const addContactCancelBtn = document.getElementById('add-contact-cancel');
    const settingsOverlayEl = document.querySelectorAll('.modal-overlay')[1];
    const settingsWindowEl = document.getElementById('settings-window');
    const settingsNameInput = document.getElementById('settings-input-name');
    const settingsIpInput = document.getElementById('settings-input-ip');
    const settingsConfirmBtn = document.getElementById('settings-confirm');
    const settingsCancelBtn = document.getElementById('settings-cancel');

    function el(tag, className, text) {
        const e = document.createElement(tag);
        if (className) e.className = className;
        if (text) e.textContent = text;
        return e;
    }

    async function fetchSelfPublicKey() {
        try {
            const resp = await fetch(api.publicKey);
            if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
            const data = await resp.json();
            selfPublicKey = data.public_key;
        } 
        catch (err) {
            console.error('Failed to fetch self public key:', err);
        }
    }

    myDomainValueEl.textContent = location.host;

    myDomainCopyBtn.addEventListener('click', async () => {
        try {
            await navigator.clipboard.writeText(location.host);
            myDomainCopyBtn.textContent = 'Copied!';
            setTimeout(() => { myDomainCopyBtn.textContent = 'Copy'; }, 2000);
        } 
        catch (e) {
            console.error('Domain copy failed:', e);
            alert('Failed to copy domain');
        }
    });

    async function fetchContacts() {
        try {
            const resp = await apiFetch(api.contacts);
            if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
            const data = await resp.json();
            return data.contacts || [];
        } 
        catch (err) {
            console.error('Failed to fetch contacts:', err);
            return [];
        }
    }

    function renderContacts(contacts) {
        lastContacts = contacts;
        contactsListEl.innerHTML = '';
        contacts.forEach(contact => {
            const contactDiv = el('div', 'contact');
            if (contact.contact_id === selectedContactId) {
                contactDiv.classList.add('selected');
            }
            const infoDiv = el('div');
            const nameDiv = el('div', 'contact-name', contact.name || '(no name)');
            const latestDiv = el('div', 'latest-message');
            if (contact.latest_message) {
                const msg = contact.latest_message;
                latestDiv.textContent = `Latest message: ${msg.plaintext}`;
                const timeSpan = el('span', 'latest-message-time', formatClockTime(msg.timestamp));
                contactDiv.appendChild(timeSpan);
            } 
            else {
                latestDiv.textContent = 'No messages yet';
            }
            infoDiv.appendChild(nameDiv);
            infoDiv.appendChild(latestDiv);
            contactDiv.appendChild(infoDiv);
            contactDiv.addEventListener('click', () => {
                const prev = contactsListEl.querySelector('.contact.selected');
                if (prev) prev.classList.remove('selected');
                contactDiv.classList.add('selected');
                selectContact(contact.contact_id, contact.name);
            });
            contactsListEl.appendChild(contactDiv);
        });
    }

    function formatClockTime(timestamp) {
        if (!timestamp) return 'unknown';
        const d = new Date(timestamp * 1000);
        const hh = String(d.getHours()).padStart(2, '0');
        const mm = String(d.getMinutes()).padStart(2, '0');
        return `${hh}:${mm}`;
    }

    function selectContact(contactId, contactName) {
        selectedContactId = contactId;
        localStorage.setItem('selectedContactId', contactId);
        contactNameTextEl.textContent = contactName || '(unknown)';
        chatHeaderEl.style.display = '';
        messageFieldEl.style.display = '';
        chatSettingsBtn.style.display = '';
        messagesListEl.innerHTML = '';
        fetchAndRenderMessages();
    }

    function clearChatArea() {
        selectedContactId = null;
        localStorage.removeItem('selectedContactId');
        contactNameTextEl.textContent = '';
        chatHeaderEl.style.display = 'none';
        messageFieldEl.style.display = 'none';
        chatSettingsBtn.style.display = 'none';
        messagesListEl.innerHTML = '<div class="chat-placeholder">Select a chat to start messaging</div>';
    }

    async function fetchMessages() {
        if (!selectedContactId) return [];
        try {
            const resp = await apiFetch(`${api.messages}?contact_id=${encodeURIComponent(selectedContactId)}`);
            if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
            const data = await resp.json();
            return data.messages || [];
        } 
        catch (err) {
            console.error('Failed to fetch messages:', err);
            return [];
        }
    }

    function renderMessages(messages) {
        messagesListEl.innerHTML = '';
        messages.forEach(msg => {
            const messageDiv = el('div', 'message');
            const isMine = msg.sender_id === selfPublicKey;
            if (isMine) messageDiv.classList.add('mine');
            else messageDiv.classList.add('theirs');
            const textDiv = el('div', 'message-text', msg.plaintext);
            messageDiv.appendChild(textDiv);
            if (isMine) {
                const statusDiv = el('div', 'message-status');
                if (msg.accepted === 1) {
                    statusDiv.textContent = '✓';
                } 
                else if (msg.accepted === 2) {
                    statusDiv.textContent = '✗';
                }
                else {
                    statusDiv.textContent = '⏳';
                }
                messageDiv.appendChild(statusDiv);
            }
            messagesListEl.appendChild(messageDiv);
        });
        messagesListEl.scrollTop = messagesListEl.scrollHeight;
    }

    async function fetchAndRenderMessages() {
        const messages = await fetchMessages();
        renderMessages(messages);
    }

    function refreshContacts() {
        fetchContacts().then(renderContacts);
    }

    async function sendMessage(text) {
        if (!selectedContactId) {
            alert('Please select a contact first');
            return;
        }
        if (!text.trim()) return;
        try {
            const resp = await apiFetch(api.sendMessage, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    recipient_id: selectedContactId,
                    text: text
                })
            });
            if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
            const data = await resp.json();
            pendingSelfMessageId = data.message_id;
            await fetchAndRenderMessages();
            messageInputEl.value = '';
        } 
        catch (err) {
            console.error('Failed to send message:', err);
            alert('Failed to send message');
        } 
        finally {
            setTimeout(() => {
                pendingSelfMessageId = null;
            }, 5000);
        }
    }

    sendButtonEl.addEventListener('click', () => {
        sendMessage(messageInputEl.value.trim());
    });
    messageInputEl.addEventListener('keypress', e => {
        if (e.key === 'Enter') {
            sendMessage(messageInputEl.value.trim());
        }
    });

    addContactBtnEl.addEventListener('click', () => {
        addContactOverlayEl.style.display = 'flex';
        addContactWindowEl.style.display = 'block';
        addContactNameInput.value = '';
        addContactIpInput.value = '';
        addContactNameInput.focus();
    });

    function hideAddContactModal() {
        addContactOverlayEl.style.display = 'none';
        addContactWindowEl.style.display = 'none';
    }

    addContactCancelBtn.addEventListener('click', hideAddContactModal);
    addContactOverlayEl.addEventListener('click', e => {
        if (e.target === addContactOverlayEl) hideAddContactModal();
    });

    addContactConfirmBtn.addEventListener('click', async () => {
        const name = addContactNameInput.value.trim();
        const addr = addContactIpInput.value.trim();
        if (!name || !addr) {
            alert('Please fill both name and address');
            return;
        }
        try {
            const pubKeyUrl = api.fetchRemotePublicKey(addr);
            const resp = await fetch(pubKeyUrl);
            if (!resp.ok) throw new Error(`Failed to fetch public key: HTTP ${resp.status}`);
            const data = await resp.json();
            const remotePubKey = data.public_key;
            if (!remotePubKey) throw new Error('No public key in response');
            const addResp = await apiFetch(api.upsertContact, {
                method: 'PUT',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    contact_id: remotePubKey,
                    name: name,
                    server_address: addr
                })
            });
            if (!addResp.ok) throw new Error(`Failed to add/update contact: HTTP ${addResp.status}`);
            hideAddContactModal();
            const contacts = await fetchContacts();
            renderContacts(contacts);
            selectContact(remotePubKey, name);
        } 
        catch (err) {
            console.error('Add contact failed:', err);
            alert(`Failed to add contact: ${err.message}`);
        }
    });

    function openSettingsFor(contact) {
        settingsNameInput.value = contact.name || '';
        settingsIpInput.value = contact.server_address || '';
        settingsWindowEl.dataset.editingContactId = contact.contact_id;
        settingsOverlayEl.style.display = 'flex';
        settingsWindowEl.style.display = 'block';
        settingsNameInput.focus();
    }

    chatSettingsBtn.addEventListener('click', () => {
        if (!selectedContactId) return;
        const contact = lastContacts.find(c => c.contact_id === selectedContactId);
        if (contact) openSettingsFor(contact);
    });

    function hideSettingsModal() {
        settingsOverlayEl.style.display = 'none';
        settingsWindowEl.style.display = 'none';
        delete settingsWindowEl.dataset.editingContactId;
    }

    settingsCancelBtn.addEventListener('click', hideSettingsModal);
    settingsOverlayEl.addEventListener('click', e => {
        if (e.target === settingsOverlayEl) hideSettingsModal();
    });

    settingsConfirmBtn.addEventListener('click', async () => {
        const name = settingsNameInput.value.trim();
        const addr = settingsIpInput.value.trim();
        if (!name || !addr) {
            alert('Please fill both name and address');
            return;
        }
        const contactId = settingsWindowEl.dataset.editingContactId;
        if (!contactId) {
            hideSettingsModal();
            return;
        }
        try {
            const updateResp = await apiFetch(api.upsertContact, {
                method: 'PUT',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    contact_id: contactId,
                    name: name,
                    server_address: addr
                })
            });
            if (!updateResp.ok) throw new Error(`Failed to update contact: HTTP ${updateResp.status}`);
            hideSettingsModal();
            const contacts = await fetchContacts();
            renderContacts(contacts);
            if (selectedContactId) {
                const stillThere = contacts.some(c => c.contact_id === selectedContactId);
                if (!stillThere) {
                    clearChatArea();
                } 
                else {
                    const updatedContact = contacts.find(c => c.contact_id === selectedContactId);
                    if (updatedContact) {
                        contactNameTextEl.textContent = updatedContact.name || '(unknown)';
                    }
                }
            }
        } 
        catch (err) {
            console.error('Update contact failed:', err);
            alert(`Failed to update contact: ${err.message}`);
        }
    });

    let ws = null;
    function initWebSocket() {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const wsUrl = `${protocol}//${window.location.host}/ws/messages`;
        ws = new WebSocket(wsUrl);
        ws.onopen = () => {
            if (authId && authSecret) {
                ws.send(JSON.stringify({ type: 'auth', client_id: authId, secret: authSecret }));
            }
        };
        ws.onclose = () => {
            if (authId && authSecret) {
                setTimeout(initWebSocket, 3000);
            }
        };
        ws.onerror = e => {
            console.error('WebSocket error:', e);
        };
        ws.onmessage = event => {
            try {
                const data = JSON.parse(event.data);
                if (data.type === 'new_message') {
                    if (pendingSelfMessageId && data.message_id === pendingSelfMessageId) {
                        pendingSelfMessageId = null;
                    } 
                    else {
                        if (selectedContactId && data.contact_id === selectedContactId) {
                            fetchAndRenderMessages();
                        }
                    }
                    refreshContacts();
                }
            } 
            catch (e) {
                console.warn('Invalid WS message:', event.data);
            }
        };
    }

    async function init() {
        if (authId && authSecret) {
            const probe = await apiFetch(api.contacts);
            if (!probe.ok) return;
        } 
        else {
            const registered = await registerClient();
            if (!registered) return;
        }
        messengerWindowEl.style.display = 'flex';
        await fetchSelfPublicKey();
        const contacts = await fetchContacts();
        renderContacts(contacts);
        const savedContactId = localStorage.getItem('selectedContactId');
        const startContact = contacts.find(c => c.contact_id === savedContactId);
        if (startContact) {
            selectContact(startContact.contact_id, startContact.name);
        } 
        else {
            clearChatArea();
        }
        setInterval(async () => {
            if (selectedContactId) {
                await fetchAndRenderMessages();
            }
            const contacts = await fetchContacts();
            renderContacts(contacts);
            if (selectedContactId) {
                const stillThere = contacts.some(c => c.contact_id === selectedContactId);
                if (!stillThere) {
                    clearChatArea();
                }
            }
        }, 5000);
        initWebSocket();
    }

    init();
})();