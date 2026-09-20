(() => {
    const baseUrl = window.location.origin;
    const api = {
        authRequest: `${baseUrl}/api/auth/request`,
        authApprove: `${baseUrl}/api/auth/approve`,
        authRequestStatus: (id) => `${baseUrl}/api/auth/request?request_id=${encodeURIComponent(id)}`,
        clients: `${baseUrl}/api/clients`,
        client: (id) => `${baseUrl}/api/clients/${encodeURIComponent(id)}`,
        publicKey: `${baseUrl}/api/public_key`,
        contacts: `${baseUrl}/api/contacts`,
        messages: `${baseUrl}/api/messages`,
        sendMessage: `${baseUrl}/api/send_message`,
        editMessage: `${baseUrl}/api/edit_message`,
        deleteMessage: `${baseUrl}/api/delete_message`,
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
    const lockRegisterEl = document.getElementById('lock-register');
    const lockDeviceNameInput = document.getElementById('lock-device-name');
    const lockRequestBtn = document.getElementById('lock-request-btn');
    const lockWaitingEl = document.getElementById('lock-waiting');
    const lockRefreshBtn = document.getElementById('lock-refresh');

    let pollTimer = null;
    function stopPolling() {
        if (pollTimer) {
            clearInterval(pollTimer);
            pollTimer = null;
        }
    }

    function showLockScreen(message, showForm) {
        stopPolling();
        lockMessageEl.textContent = message || '';
        lockRegisterEl.style.display = showForm ? 'flex' : 'none';
        lockDeviceNameInput.style.display = showForm ? '' : 'none';
        lockWaitingEl.style.display = 'none';
        lockScreenEl.style.display = 'flex';
    }

    function saveCreds(clientId, secret) {
        authId = clientId;
        authSecret = secret;
        localStorage.setItem(AUTH_ID_KEY, clientId);
        localStorage.setItem(AUTH_SECRET_KEY, secret);
    }

    function defaultDeviceName() {
        const ua = navigator.userAgent;
        let os = 'desktop';
        if (/Windows/.test(ua)) os = 'Windows';
        else if (/Mac OS X/.test(ua)) os = 'macOS';
        else if (/Android/.test(ua)) os = 'Android';
        else if (/iPhone|iPad|iPod/.test(ua)) os = 'iOS';
        else if (/Linux/.test(ua)) os = 'Linux';
        const browsers = [['Edge', /Edg\//], ['Chrome', /Chrome\//], ['Firefox', /Firefox\//], ['Safari', /Safari\//]];
        const b = browsers.find(([, re]) => re.test(ua));
        return b ? `${b[0]} on ${os}` : `Browser on ${os}`;
    }

    function startPolling(requestId, deviceName) {
        lockWaitingEl.style.display = 'block';
        lockWaitingEl.textContent = `Waiting for the owner to approve "${deviceName}"...`;
        pollTimer = setInterval(async () => {
            try {
                const resp = await fetch(api.authRequestStatus(requestId));
                if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
                const data = await resp.json();
                if (data.status === 'approved') {
                    stopPolling();
                    saveCreds(data.client_id, data.secret);
                    location.reload();
                } 
                else if (data.status === 'expired' || data.status === 'unknown') {
                    stopPolling();
                    showLockScreen('The request expired or was cancelled. Request access again.', true);
                }
            } 
            catch (e) {
                console.warn('Polling auth request failed:', e);
            }
        }, 2000);
    }

    async function requestAccess(name) {
        const resp = await fetch(api.authRequest, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ name })
        });
        if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
        const data = await resp.json();
        if (data.auto) {
            saveCreds(data.client_id, data.secret);
            return { auto: true };
        }
        return { auto: false, request_id: data.request_id };
    }

    async function startLockFlow() {
        const defaultName = defaultDeviceName();
        let res;
        try {
            res = await requestAccess(defaultName);
        } 
        catch (err) {
            console.error('Failed to request access:', err);
            showLockScreen('Failed to connect to the server.', false);
            return false;
        }
        if (res.auto) return true;
        lockDeviceNameInput.value = defaultName;
        showLockScreen('Enter a device name to request access.', true);
        startPolling(res.request_id, defaultName);
        return false;
    }

    lockRequestBtn.addEventListener('click', async () => {
        lockRequestBtn.disabled = true;
        try {
            const name = lockDeviceNameInput.value.trim() || defaultDeviceName();
            stopPolling();
            const res = await requestAccess(name);
            if (res.auto) {
                location.reload();
            } 
            else {
                startPolling(res.request_id, name);
            }
        } 
        catch (err) {
            console.error('Failed to request access:', err);
            showLockScreen('Failed to connect to the server.', false);
        } 
        finally {
            lockRequestBtn.disabled = false;
        }
    });

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

    let selfPublicKey = null;
    let selectedContactId = null;
    let lastContacts = [];
    let pendingSelfMessageId = null;
    let editingMessage = null;

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
    const editBarEl = document.getElementById('edit-bar');
    const editBarCancelBtn = document.getElementById('edit-bar-cancel');
    const addContactBtnEl = document.getElementById('add-contact-button');
    const addContactOverlayEl = document.getElementById('add-contact-overlay');
    const addContactWindowEl = document.getElementById('add-contact-window');
    const addContactNameInput = document.getElementById('add-contact-input-name');
    const addContactIpInput = document.getElementById('add-contact-input-ip');
    const addContactConfirmBtn = document.getElementById('add-contact-confirm');
    const addContactCancelBtn = document.getElementById('add-contact-cancel');
    const settingsOverlayEl = document.getElementById('settings-overlay');
    const settingsWindowEl = document.getElementById('settings-window');
    const settingsNameInput = document.getElementById('settings-input-name');
    const settingsIpInput = document.getElementById('settings-input-ip');
    const settingsConfirmBtn = document.getElementById('settings-confirm');
    const settingsCancelBtn = document.getElementById('settings-cancel');

    const devicesButtonEl = document.getElementById('devices-button');
    const approvalOverlayEl = document.getElementById('approval-overlay');
    const approvalWindowEl = document.getElementById('approval-window');
    const approvalDeviceNameEl = document.getElementById('approval-device-name');
    const approvalApproveBtn = document.getElementById('approval-approve-btn');
    const approvalRejectBtn = document.getElementById('approval-reject-btn');
    const devicesOverlayEl = document.getElementById('devices-overlay');
    const devicesWindowEl = document.getElementById('devices-window');
    const devicesListEl = document.getElementById('devices-list');
    const devicesCloseBtn = document.getElementById('devices-close');

    function openApprovalDialog(requestId, deviceName) {
        approvalDeviceNameEl.textContent = `Device "${deviceName}" wants to connect to this node.`;
        approvalWindowEl.dataset.requestId = requestId;
        approvalOverlayEl.style.display = 'flex';
        approvalWindowEl.style.display = 'block';
    }

    function closeApprovalDialog() {
        approvalOverlayEl.style.display = 'none';
        approvalWindowEl.style.display = 'none';
        delete approvalWindowEl.dataset.requestId;
    }

    approvalOverlayEl.addEventListener('click', e => {
        if (e.target === approvalOverlayEl) closeApprovalDialog();
    });

    async function submitApproval(allow) {
        const requestId = approvalWindowEl.dataset.requestId;
        if (!requestId) {
            closeApprovalDialog();
            return;
        }
        approvalApproveBtn.disabled = true;
        approvalRejectBtn.disabled = true;
        try {
            const resp = await apiFetch(api.authApprove, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ request_id: requestId, allow })
            });
            if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
            closeApprovalDialog();
        } 
        catch (err) {
            console.error('Failed to submit approval:', err);
            alert(`Failed to submit approval: ${err.message}`);
        } 
        finally {
            approvalApproveBtn.disabled = false;
            approvalRejectBtn.disabled = false;
        }
    }

    approvalApproveBtn.addEventListener('click', () => submitApproval(true));
    approvalRejectBtn.addEventListener('click', () => submitApproval(false));

    function closeDevices() {
        devicesOverlayEl.style.display = 'none';
        devicesWindowEl.style.display = 'none';
    }

    devicesCloseBtn.addEventListener('click', closeDevices);
    devicesOverlayEl.addEventListener('click', e => {
        if (e.target === devicesOverlayEl) closeDevices();
    });

    function renderDevices(clients) {
        devicesListEl.innerHTML = '';
        (clients || []).forEach(c => {
            const row = el('div', 'device-row');
            const infoBlock = el('div', 'device-info-block');
            infoBlock.appendChild(el('div', 'device-info', c.device_name || c.client_id));
            if (c.device_name) {
                infoBlock.appendChild(el('div', 'device-added', c.client_id));
            }
            infoBlock.appendChild(el('div', 'device-added', `added ${formatClockTime(c.created_at)}`));
            row.appendChild(infoBlock);
            if (c.client_id === authId) {
                row.appendChild(el('div', 'device-current', 'this device'));
            } 
            else {
                const revokeBtn = el('button', 'device-revoke', 'Revoke');
                revokeBtn.addEventListener('click', () => revokeDevice(c.client_id));
                row.appendChild(revokeBtn);
            }
            devicesListEl.appendChild(row);
        });
    }

    async function openDevices() {
        devicesListEl.innerHTML = '<div class="device-info" style="color:#8b93a3;">Loading...</div>';
        devicesOverlayEl.style.display = 'flex';
        devicesWindowEl.style.display = 'block';
        try {
            const resp = await apiFetch(api.clients);
            if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
            const data = await resp.json();
            renderDevices(data.clients || []);
        } 
        catch (err) {
            console.error('Failed to load devices:', err);
            devicesListEl.innerHTML = '<div class="device-info" style="color:#e7e9ee;">Failed to load devices.</div>';
        }
    }

    devicesButtonEl.addEventListener('click', openDevices);

    async function revokeDevice(clientId) {
        try {
            const resp = await apiFetch(api.client(clientId), { method: 'DELETE' });
            if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
            openDevices();
        } 
        catch (err) {
            console.error('Failed to revoke device:', err);
            alert('Failed to revoke device');
        }
    }

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
                if (msg.accepted === 3) {
                    latestDiv.textContent = 'No messages yet';
                } 
                else {
                    latestDiv.textContent = `Latest message: ${msg.plaintext}`;
                    const timeSpan = el('span', 'latest-message-time', formatClockTime(msg.timestamp));
                    contactDiv.appendChild(timeSpan);
                }
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
        cancelEditing();
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
        cancelEditing();
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
            if (msg.accepted === 3) return;
            const messageDiv = el('div', 'message');
            const isMine = msg.sender_id === selfPublicKey;
            if (isMine) messageDiv.classList.add('mine');
            else messageDiv.classList.add('theirs');
            if (editingMessage && msg.message_id === editingMessage.message_id) {
                messageDiv.classList.add('editing');
            }
            const textDiv = el('div', 'message-text', msg.plaintext);
            messageDiv.appendChild(textDiv);
            messageDiv.addEventListener('contextmenu', e => {
                e.preventDefault();
                showContextMenu(e.clientX, e.clientY, msg);
            });

            const metaRow = el('div', 'message-meta');
            if (msg.edited_at) {
                metaRow.appendChild(el('span', 'message-edited', 'edited'));
            }
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
                metaRow.appendChild(statusDiv);
            }
            if (metaRow.childNodes.length > 0) {
                messageDiv.appendChild(metaRow);
            }
            messagesListEl.appendChild(messageDiv);
        });
        messagesListEl.scrollTop = messagesListEl.scrollHeight;
    }

    async function editMessage(messageId, text) {
        try {
            const resp = await apiFetch(api.editMessage, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    message_id: messageId,
                    text: text
                })
            });
            if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
            cancelEditing();
            await fetchAndRenderMessages();
            refreshContacts();
        }
        catch (err) {
            console.error('Failed to edit message:', err);
            alert('Failed to edit message');
        }
    }

    function cancelEditing() {
        editingMessage = null;
        editBarEl.style.display = 'none';
        messageInputEl.value = '';
    }

    function startEditMessage(msg) {
        if (!isMine(msg)) return;
        editingMessage = msg;
        messageInputEl.value = msg.plaintext;
        editBarEl.style.display = 'flex';
        messageInputEl.focus();
        messageInputEl.setSelectionRange(messageInputEl.value.length, messageInputEl.value.length);
        fetchAndRenderMessages().then(() => {
            messagesListEl.scrollTop = messagesListEl.scrollHeight;
        });
    }

    async function deleteMessage(messageId) {
        try {
            const resp = await apiFetch(api.deleteMessage, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ message_id: messageId })
            });
            if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
            await fetchAndRenderMessages();
            refreshContacts();
        }
        catch (err) {
            console.error('Failed to delete message:', err);
            alert('Failed to delete message');
        }
    }

    function isMine(msg) {
        return msg.sender_id === selfPublicKey;
    }

    const contextMenuEl = el('div', 'context-menu');
    document.body.appendChild(contextMenuEl);

    function hideContextMenu() {
        contextMenuEl.style.display = 'none';
        contextMenuEl.innerHTML = '';
    }

    function menuIcon(svg) {
        const span = el('span', 'context-menu-icon');
        span.innerHTML = svg;
        return span;
    }

    function showContextMenu(x, y, msg) {
        contextMenuEl.innerHTML = '';
        const copyBtn = el('div', 'context-menu-item', 'Copy');
        copyBtn.prepend(menuIcon(
            '<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="9" y="9" width="13" height="13" rx="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg>'
        ));
        copyBtn.addEventListener('click', async () => {
            hideContextMenu();
            try {
                await navigator.clipboard.writeText(msg.plaintext);
            }
            catch (e) {
                console.error('Failed to copy message:', e);
            }
        });
        contextMenuEl.appendChild(copyBtn);
        if (isMine(msg)) {
            const editBtn = el('div', 'context-menu-item', 'Edit');
            editBtn.prepend(menuIcon(
                '<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M17 3a2.83 2.83 0 1 1 4 4L7.5 20.5 2 22l1.5-5.5L17 3z"/></svg>'
            ));
            editBtn.addEventListener('click', () => {
                hideContextMenu();
                startEditMessage(msg);
            });
            contextMenuEl.appendChild(editBtn);
            const deleteBtn = el('div', 'context-menu-item context-menu-danger', 'Delete');
            deleteBtn.prepend(menuIcon(
                '<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 6h18M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2m3 0v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6h14z"/></svg>'
            ));
            deleteBtn.addEventListener('click', async () => {
                hideContextMenu();
                if (!confirm('Delete this message?')) return;
                await deleteMessage(msg.message_id);
            });
            contextMenuEl.appendChild(deleteBtn);
        }
        contextMenuEl.style.display = 'block';
        const menuWidth = contextMenuEl.offsetWidth;
        const menuHeight = contextMenuEl.offsetHeight;
        const left = x + menuWidth > window.innerWidth ? x - menuWidth - 4 : x;
        const top = y + menuHeight > window.innerHeight ? y - menuHeight - 4 : y;
        contextMenuEl.style.left = `${Math.max(0, left)}px`;
        contextMenuEl.style.top = `${Math.max(0, top)}px`;
    }

    document.addEventListener('click', e => {
        if (!contextMenuEl.contains(e.target)) hideContextMenu();
    });
    document.addEventListener('contextmenu', e => {
        if (!e.target.closest('.message')) hideContextMenu();
    });
    document.addEventListener('scroll', hideContextMenu, true);
    window.addEventListener('resize', hideContextMenu);

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

    function doSend() {
        const text = messageInputEl.value.trim();
        if (!text) return;
        if (editingMessage) {
            if (text === editingMessage.plaintext) {
                cancelEditing();
                return;
            }
            editMessage(editingMessage.message_id, text);
        }
        else {
            sendMessage(text);
        }
    }

    sendButtonEl.addEventListener('click', doSend);
    messageInputEl.addEventListener('keypress', e => {
        if (e.key === 'Enter') {
            e.preventDefault();
            doSend();
        }
    });
    messageInputEl.addEventListener('keydown', e => {
        if (e.key === 'Escape' && editingMessage) {
            cancelEditing();
        }
    });
    editBarCancelBtn.addEventListener('click', cancelEditing);

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
                else if (data.type === 'edit_message') {
                    if (selectedContactId && data.contact_id === selectedContactId) {
                        fetchAndRenderMessages();
                    }
                    refreshContacts();
                }
                else if (data.type === 'delete_message') {
                    if (selectedContactId && data.contact_id === selectedContactId) {
                        fetchAndRenderMessages();
                    }
                    refreshContacts();
                }
                else if (data.type === 'auth_request') {
                    openApprovalDialog(data.request_id, data.device_name || 'New device');
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
            if (!probe.ok) {
                if (probe.status !== 401) {
                    showLockScreen('Failed to connect to the server.', false);
                }
                return;
            }
        } 
        else {
            const granted = await startLockFlow();
            if (!granted) return;
        }
        messengerWindowEl.style.display = 'flex';
        lockScreenEl.style.display = 'none';
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