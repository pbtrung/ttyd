import { bind } from 'decko';
import { Component, h } from 'preact';
import { GhosttyTerminal, GhosttyOptions } from './ghostty';

import { Modal } from '../modal';

interface Props extends GhosttyOptions {
    id: string;
}

interface State {
    modal: boolean;
}

export class Terminal extends Component<Props, State> {
    private container: HTMLElement;
    private terminal: GhosttyTerminal;
    private mounted = false;

    constructor(props: Props) {
        super();
        this.terminal = new GhosttyTerminal(props, this.showModal);
    }

    async componentDidMount() {
        this.mounted = true;
        try {
            await this.terminal.refreshToken();
            await this.terminal.open(this.container);
            this.terminal.connect();
        } catch (error) {
            if (!this.mounted) return;
            console.error('[ttyd] terminal initialization failed', error);
            const message = document.createElement('p');
            message.textContent = 'Unable to initialize terminal. Reload the page to retry.';
            this.container.appendChild(message);
        }
    }

    componentWillUnmount() {
        this.mounted = false;
        this.terminal.destroy();
    }

    render({ id }: Props, { modal }: State) {
        return (
            <div id={id}>
                <div class="terminal" ref={c => (this.container = c as HTMLElement)} />
                <Modal show={modal}>
                    <label class="file-label">
                        <input onChange={this.sendFile} class="file-input" type="file" multiple />
                        <span class="file-cta">Choose files…</span>
                    </label>
                </Modal>
            </div>
        );
    }

    @bind
    showModal() {
        this.setState({ modal: true });
    }

    @bind
    sendFile(event: Event) {
        this.setState({ modal: false });
        const files = (event.target as HTMLInputElement).files;
        if (files) this.terminal.sendFile(files);
    }
}
