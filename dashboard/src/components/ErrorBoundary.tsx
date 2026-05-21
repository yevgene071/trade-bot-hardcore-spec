import * as React from 'react';
import { AlertTriangle, RefreshCw } from 'lucide-react';

interface Props {
  children: React.ReactNode;
  fallbackLabel?: string;
}

interface State {
  hasError: boolean;
  error: Error | null;
  retryKey: number;
}

export class ErrorBoundary extends React.Component<Props, State> {
  constructor(props: Props) {
    super(props);
    this.state = { hasError: false, error: null, retryKey: 0 };
  }

  static getDerivedStateFromError(error: Error): Partial<State> {
    return { hasError: true, error };
  }

  componentDidCatch(error: Error, info: React.ErrorInfo) {
    console.error('[ErrorBoundary]', error.message, info.componentStack);
  }

  handleRetry = () => {
    this.setState(s => ({ hasError: false, error: null, retryKey: s.retryKey + 1 }));
  };

  render() {
    if (this.state.hasError) {
      const label = this.props.fallbackLabel ?? 'Chart';
      return (
        <div className="w-full h-full flex flex-col items-center justify-center bg-[#04060c] text-[#7b859c] gap-3 px-6">
          <AlertTriangle size={28} className="text-[#c08828]" />
          <div className="text-center">
            <div className="text-[11px] font-semibold text-[#d8e8f8] mb-1">
              {label} render error
            </div>
            <div className="text-[9px] font-mono text-[#3c4e62] max-w-[320px] leading-relaxed">
              {this.state.error?.message ?? 'Unknown error'}
            </div>
          </div>
          <button
            onClick={this.handleRetry}
            className="mt-2 flex items-center gap-1.5 px-4 py-1.5 bg-[#c08828]/15 text-[#c08828] hover:bg-[#c08828] hover:text-[#04060c] rounded text-[9px] font-bold uppercase tracking-widest transition-colors border border-[#c08828]/30"
          >
            <RefreshCw size={11} />
            Retry
          </button>
        </div>
      );
    }

    // Key changes on retry to force complete unmount+remount of children
    return <React.Fragment key={this.state.retryKey}>{this.props.children}</React.Fragment>;
  }
}
